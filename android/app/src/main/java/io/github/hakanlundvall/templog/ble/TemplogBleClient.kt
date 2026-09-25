package io.github.hakanlundvall.templog.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import java.util.ArrayDeque
import java.util.UUID

/** Where the single BLE link to the ESP32 currently is in its lifecycle. */
enum class ConnectionState {
    /** Bluetooth is off, or permissions are missing. */
    UNAVAILABLE,

    /** Between connection attempts, waiting out the reconnect backoff. */
    IDLE,

    SCANNING,
    CONNECTING,

    /** Waiting for the "Just Works" pairing to complete. */
    PAIRING,

    DISCOVERING,

    /** Subscribed and holding a current telemetry snapshot. */
    READY,
}

class CommandException(message: String) : RuntimeException(message)

/**
 * Keeps a single BLE connection to the templog ESP32 alive, mirroring what
 * [rpi/templog_ble/ble_client.py] does on the Raspberry Pi: connect (scanning
 * by advertised name when the address is not known yet), bond, subscribe to
 * both notify characteristics, and reconnect with exponential backoff whenever
 * the link drops.
 *
 * Every GATT operation is funnelled through a queue on the main thread,
 * because Android's stack only tolerates one outstanding operation per
 * connection and silently drops the rest.
 */
@SuppressLint("MissingPermission")
class TemplogBleClient(context: Context) {

    private val appContext = context.applicationContext
    private val handler = Handler(Looper.getMainLooper())

    private val bluetoothManager =
        appContext.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager?
    private val adapter: BluetoothAdapter? = bluetoothManager?.adapter

    private val _connectionState = MutableStateFlow(ConnectionState.IDLE)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _telemetry = MutableStateFlow<Telemetry?>(null)
    val telemetry: StateFlow<Telemetry?> = _telemetry.asStateFlow()

    /** The newest copy of each reported document, keyed by characteristic. */
    private val documents = linkedMapOf<UUID, JSONObject>()

    /** Guards against rediscovering in a loop on a device that really is old. */
    private var cacheRefreshed = false

    private val _deviceAddress = MutableStateFlow<String?>(null)
    val deviceAddress: StateFlow<String?> = _deviceAddress.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    private val _statusEvents = MutableSharedFlow<CommandStatus>(extraBufferCapacity = 16)
    val statusEvents: SharedFlow<CommandStatus> = _statusEvents.asSharedFlow()

    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private var running = false
    private var scanning = false
    private var backoffMs = MIN_BACKOFF_MS

    private val queue = ArrayDeque<Op>()
    private var pending: Op? = null

    /** Operations rejected for lack of encryption, replayed once bonding lands. */
    private val awaitingBond = ArrayDeque<Op>()

    /** Command writers waiting for a status reply, keyed by the "cmd" field. */
    private val pendingCommands = mutableMapOf<String, MutableList<CompletableDeferred<CommandStatus>>>()

    private var bondReceiverRegistered = false

    /** Negotiated ATT MTU, which decides how large a firmware chunk can be. */
    private var mtu = DEFAULT_MTU

    // ---------------------------------------------------------------- lifecycle

    /** Starts (or restarts) the connect/reconnect loop. Safe to call repeatedly. */
    fun start() {
        handler.post {
            if (running) return@post
            running = true
            registerBondReceiver()
            backoffMs = MIN_BACKOFF_MS
            connectNow()
        }
    }

    /** Tears the link down and stops reconnecting. */
    fun stop() {
        handler.post {
            running = false
            handler.removeCallbacks(reconnectRunnable)
            stopScan()
            teardownGatt()
            failAllPendingCommands("disconnected")
            unregisterBondReceiver()
            _connectionState.value = ConnectionState.IDLE
        }
    }

    /** Drops the current link so the next attempt starts from a clean slate. */
    fun reconnect() {
        handler.post {
            if (!running) {
                start()
                return@post
            }
            teardownGatt()
            backoffMs = MIN_BACKOFF_MS
            connectNow()
        }
    }

    /**
     * Forgets the remembered address so the next attempt scans again. Useful
     * after the ESP32 has been re-flashed and advertises a new random address.
     */
    fun forgetDevice() {
        handler.post {
            _deviceAddress.value = null
            device = null
            reconnect()
        }
    }

    // ----------------------------------------------------------------- commands

    /**
     * Writes a JSON command and waits for the matching status reply, the same
     * contract as `BleClient.send_command` on the Raspberry Pi.
     */
    suspend fun sendCommand(
        command: JSONObject,
        timeoutMs: Long = COMMAND_TIMEOUT_MS,
    ): CommandStatus {
        if (_connectionState.value != ConnectionState.READY) {
            throw CommandException("not connected to device")
        }
        val cmd = command.optString("cmd")
        val deferred = CompletableDeferred<CommandStatus>()

        handler.post {
            pendingCommands.getOrPut(cmd) { mutableListOf() }.add(deferred)
            enqueue(
                Op.Write(
                    uuid = Protocol.COMMAND_CHAR_UUID,
                    value = command.toString().toByteArray(Charsets.UTF_8),
                    onFailure = { reason ->
                        pendingCommands[cmd]?.remove(deferred)
                        deferred.completeExceptionally(CommandException(reason))
                    },
                ),
            )
        }

        return try {
            withTimeout(timeoutMs) { deferred.await() }.also { status ->
                if (!status.ok) throw CommandException(status.error ?: "command rejected")
            }
        } catch (e: TimeoutCancellationException) {
            handler.post { pendingCommands[cmd]?.remove(deferred) }
            throw CommandException("timed out waiting for status reply to $cmd")
        }
    }

    /**
     * Pushes a whole firmware image to the device, chunk by chunk, after an
     * ota_ble_begin command has been accepted. Each chunk carries its offset,
     * so the device notices at once if one goes missing.
     *
     * Chunks go out as writes without response, but still one at a time
     * through the GATT queue: the completion callback is what paces the
     * transfer, and it is also what reports a link that has gone away.
     * [onProgress] is called with the number of bytes accepted so far.
     */
    suspend fun sendFirmware(image: ByteArray, onProgress: (Int) -> Unit) {
        val payloadSize = (mtu - ATT_WRITE_OVERHEAD - Protocol.FIRMWARE_CHUNK_HEADER)
            .coerceIn(MIN_CHUNK_PAYLOAD, Protocol.FIRMWARE_CHUNK_MAX - Protocol.FIRMWARE_CHUNK_HEADER)

        var offset = 0
        while (offset < image.size) {
            if (_connectionState.value != ConnectionState.READY) {
                throw CommandException("lost the connection after $offset of ${image.size} bytes")
            }
            val length = minOf(payloadSize, image.size - offset)
            val chunk = ByteArray(Protocol.FIRMWARE_CHUNK_HEADER + length)
            chunk[0] = (offset and 0xFF).toByte()
            chunk[1] = ((offset ushr 8) and 0xFF).toByte()
            chunk[2] = ((offset ushr 16) and 0xFF).toByte()
            chunk[3] = ((offset ushr 24) and 0xFF).toByte()
            image.copyInto(chunk, Protocol.FIRMWARE_CHUNK_HEADER, offset, offset + length)

            val written = CompletableDeferred<Unit>()
            handler.post {
                enqueue(
                    Op.Write(
                        uuid = Protocol.FIRMWARE_CHAR_UUID,
                        value = chunk,
                        noResponse = true,
                        onSuccess = { written.complete(Unit) },
                        onFailure = { reason -> written.completeExceptionally(CommandException(reason)) },
                    ),
                )
            }
            try {
                withTimeout(CHUNK_TIMEOUT_MS) { written.await() }
            } catch (e: TimeoutCancellationException) {
                throw CommandException("timed out sending firmware at $offset bytes")
            }

            offset += length
            onProgress(offset)
        }
    }

    /** Re-reads everything the device publishes, without waiting for a notification. */
    fun refreshTelemetry() {
        handler.post {
            if (_connectionState.value == ConnectionState.READY) {
                for (uuid in REPORT_CHARS) enqueue(Op.Read(uuid))
            }
        }
    }

    fun clearError() {
        _lastError.value = null
    }

    // ------------------------------------------------------------ connect logic

    private val reconnectRunnable = Runnable { connectNow() }

    private fun scheduleReconnect() {
        if (!running) return
        _connectionState.value = ConnectionState.IDLE
        Log.i(TAG, "reconnecting in ${backoffMs}ms")
        handler.removeCallbacks(reconnectRunnable)
        handler.postDelayed(reconnectRunnable, backoffMs)
        backoffMs = (backoffMs * 2).coerceAtMost(MAX_BACKOFF_MS)
    }

    private fun connectNow() {
        if (!running) return
        handler.removeCallbacks(reconnectRunnable)

        val adapter = this.adapter
        if (adapter == null || !adapter.isEnabled) {
            _connectionState.value = ConnectionState.UNAVAILABLE
            _lastError.value = "Bluetooth is turned off"
            scheduleReconnect()
            return
        }
        if (!hasConnectPermission()) {
            _connectionState.value = ConnectionState.UNAVAILABLE
            _lastError.value = "Bluetooth permission not granted"
            return
        }

        val known = device ?: resolveKnownDevice(adapter)
        if (known != null) {
            device = known
            _deviceAddress.value = known.address
            connectGatt(known)
        } else {
            startScan()
        }
    }

    /**
     * Looks for an already bonded templog, so a reconnect can skip scanning
     * entirely (the equivalent of passing `--address` to the Pi service).
     */
    private fun resolveKnownDevice(adapter: BluetoothAdapter): BluetoothDevice? {
        val remembered = _deviceAddress.value
        if (remembered != null) {
            return runCatching { adapter.getRemoteDevice(remembered) }.getOrNull()
        }
        return runCatching {
            adapter.bondedDevices?.firstOrNull { it.name == Protocol.DEVICE_NAME }
        }.getOrNull()
    }

    private fun connectGatt(target: BluetoothDevice) {
        teardownGatt()
        _connectionState.value = ConnectionState.CONNECTING
        Log.i(TAG, "connecting to ${target.address}")
        gatt = target.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        if (gatt == null) {
            _lastError.value = "could not open a GATT connection"
            scheduleReconnect()
        }
    }

    private fun teardownGatt() {
        queue.clear()
        pending = null
        awaitingBond.clear()
        // Documents from the old connection would otherwise be merged with
        // the new one's, which matters if the device was reconfigured or
        // reflashed while it was away.
        documents.clear()
        cacheRefreshed = false
        gatt?.let {
            runCatching { it.disconnect() }
            runCatching { it.close() }
        }
        gatt = null
    }

    // -------------------------------------------------------------------- scan

    private val scanTimeoutRunnable = Runnable {
        if (scanning) {
            stopScan()
            _lastError.value = "device \"${Protocol.DEVICE_NAME}\" not found during scan"
            scheduleReconnect()
        }
    }

    private fun startScan() {
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null || !hasScanPermission()) {
            _connectionState.value = ConnectionState.UNAVAILABLE
            _lastError.value = "Bluetooth scan permission not granted"
            return
        }
        _connectionState.value = ConnectionState.SCANNING
        Log.i(TAG, "scanning for ${Protocol.DEVICE_NAME}")

        // The firmware advertises its name but not the service UUID, so the
        // name is the only thing we can filter on.
        val filters = listOf(ScanFilter.Builder().setDeviceName(Protocol.DEVICE_NAME).build())
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanning = true
        scanner.startScan(filters, settings, scanCallback)
        handler.postDelayed(scanTimeoutRunnable, SCAN_TIMEOUT_MS)
    }

    private fun stopScan() {
        handler.removeCallbacks(scanTimeoutRunnable)
        if (!scanning) return
        scanning = false
        if (hasScanPermission()) {
            runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            handler.post {
                if (!scanning) return@post
                stopScan()
                device = result.device
                _deviceAddress.value = result.device.address
                connectGatt(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            handler.post {
                scanning = false
                handler.removeCallbacks(scanTimeoutRunnable)
                _lastError.value = "BLE scan failed (error $errorCode)"
                scheduleReconnect()
            }
        }
    }

    // ------------------------------------------------------------- GATT queue

    private sealed interface Op {
        data class Read(val uuid: UUID) : Op
        class Write(
            val uuid: UUID,
            val value: ByteArray,
            val noResponse: Boolean = false,
            val onSuccess: (() -> Unit)? = null,
            val onFailure: ((String) -> Unit)? = null,
        ) : Op {
            override fun toString(): String = "Write($uuid, ${value.size} bytes)"
        }
        data class Subscribe(val uuid: UUID) : Op
        data class RequestMtu(val mtu: Int) : Op
    }

    private fun enqueue(op: Op) {
        queue.add(op)
        drain()
    }

    private fun drain() {
        if (pending != null) return
        val gatt = this.gatt ?: return
        val op = queue.poll() ?: return
        pending = op

        val started = when (op) {
            is Op.RequestMtu -> gatt.requestMtu(op.mtu)
            is Op.Read -> characteristic(op.uuid)?.let { gatt.readCharacteristic(it) } ?: false
            is Op.Subscribe -> subscribe(gatt, op.uuid)
            is Op.Write -> write(gatt, op)
        }

        if (!started) {
            Log.w(TAG, "failed to start $op")
            failOp(op, "could not start GATT operation")
            pending = null
            drain()
        }
    }

    private fun opComplete() {
        pending = null
        drain()
    }

    private fun failOp(op: Op, reason: String) {
        if (op is Op.Write) op.onFailure?.invoke(reason)
    }

    private fun succeedOp(op: Op?) {
        if (op is Op.Write) op.onSuccess?.invoke()
    }

    private fun characteristic(uuid: UUID): BluetoothGattCharacteristic? =
        gatt?.getService(Protocol.SERVICE_UUID)?.getCharacteristic(uuid)

    /**
     * Clears Android's cached copy of this device's GATT database. There is no
     * public API for it, only a hidden `refresh()`, and recent Android blocks
     * that for an app targeting a modern SDK - so this often fails, which is
     * harmless: it just leaves the cache in place, and the user clears it by
     * forgetting the device in Bluetooth settings.
     */
    private fun refreshGattCache(gatt: BluetoothGatt): Boolean = runCatching {
        gatt.javaClass.getMethod("refresh").invoke(gatt) as? Boolean ?: false
    }.getOrElse {
        Log.w(TAG, "could not refresh the cached GATT database", it)
        false
    }

    private fun subscribe(gatt: BluetoothGatt, uuid: UUID): Boolean {
        val characteristic = characteristic(uuid) ?: return false
        if (!gatt.setCharacteristicNotification(characteristic, true)) return false
        val cccd = characteristic.getDescriptor(Protocol.CCCD_UUID) ?: return false
        val enable = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(cccd, enable) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                cccd.value = enable
                gatt.writeDescriptor(cccd)
            }
        }
    }

    private fun write(gatt: BluetoothGatt, op: Op.Write): Boolean {
        val characteristic = characteristic(op.uuid) ?: return false
        val type = if (op.noResponse) {
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        } else {
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        }
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(characteristic, op.value, type) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                characteristic.writeType = type
                characteristic.value = op.value
                gatt.writeCharacteristic(characteristic)
            }
        }
    }

    /**
     * True when the peripheral refused the operation because the link is not
     * encrypted yet. Android starts pairing on its own in that case, so the
     * operation only has to be replayed once the bond exists.
     */
    private fun needsBonding(status: Int): Boolean =
        status == GATT_INSUFFICIENT_AUTHENTICATION || status == GATT_INSUFFICIENT_ENCRYPTION

    private fun deferUntilBonded(op: Op) {
        _connectionState.value = ConnectionState.PAIRING
        awaitingBond.add(op)
        // Android normally starts pairing itself when it sees these errors;
        // asking explicitly covers the stacks that do not.
        device?.let { if (it.bondState == BluetoothDevice.BOND_NONE) runCatching { it.createBond() } }
    }

    // ---------------------------------------------------------- GATT callbacks

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            handler.post {
                if (g !== gatt) {
                    runCatching { g.close() }
                    return@post
                }
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        Log.i(TAG, "connected, negotiating MTU")
                        _connectionState.value = ConnectionState.DISCOVERING
                        queue.clear()
                        pending = null
                        enqueue(Op.RequestMtu(PREFERRED_MTU))
                    }

                    BluetoothProfile.STATE_DISCONNECTED -> {
                        Log.i(TAG, "disconnected; status=$status")
                        teardownGatt()
                        failAllPendingCommands("link dropped")
                        if (status != BluetoothGatt.GATT_SUCCESS) {
                            _lastError.value = "disconnected (status $status)"
                        }
                        scheduleReconnect()
                    }
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            handler.post {
                if (g !== gatt) return@post
                Log.i(TAG, "mtu=$mtu status=$status")
                if (status == BluetoothGatt.GATT_SUCCESS) this@TemplogBleClient.mtu = mtu
                opComplete()
                if (!g.discoverServices()) {
                    _lastError.value = "service discovery could not be started"
                    teardownGatt()
                    scheduleReconnect()
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            handler.post {
                if (g !== gatt) return@post
                val service = g.getService(Protocol.SERVICE_UUID)
                if (status != BluetoothGatt.GATT_SUCCESS || service == null) {
                    _lastError.value = "templog GATT service not found on this device"
                    teardownGatt()
                    scheduleReconnect()
                    return@post
                }
                // Android caches a bonded device's service list and keeps
                // serving it after the device has been updated, so a
                // characteristic a newer firmware added simply is not there.
                // The device says so itself with a Service Changed
                // indication, which the Bluetooth stack acts on without the
                // app's help; this is the fallback for a phone that was
                // already bonded before the firmware learned to send it.
                if (REPORT_CHARS.any { service.getCharacteristic(it) == null }) {
                    if (!cacheRefreshed) {
                        cacheRefreshed = true
                        Log.i(TAG, "characteristics missing; refreshing the cached GATT database")
                        refreshGattCache(g)
                        g.discoverServices()
                        return@post
                    }
                    Log.w(TAG, "device is missing characteristics this app expects")
                }
                // Subscribing touches an encrypted characteristic, which is
                // what triggers pairing on a fresh device.
                for (uuid in REPORT_CHARS) enqueue(Op.Subscribe(uuid))
                enqueue(Op.Subscribe(Protocol.STATUS_CHAR_UUID))
                // The settings characteristic only notifies when it changes,
                // so it has to be read once here or it would stay unknown.
                for (uuid in REPORT_CHARS) enqueue(Op.Read(uuid))
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            handler.post {
                if (g !== gatt) return@post
                val op = pending
                if (status != BluetoothGatt.GATT_SUCCESS && needsBonding(status) && op != null) {
                    deferUntilBonded(op)
                    opComplete()
                    return@post
                }
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    Log.w(TAG, "descriptor write failed: $status")
                }
                opComplete()
            }
        }

        /**
         * The device says its GATT database changed, which is how a firmware
         * update announces new characteristics to an already bonded phone.
         * Only delivered from Android 12 onwards.
         */
        override fun onServiceChanged(g: BluetoothGatt) {
            handler.post {
                if (g !== gatt) return@post
                Log.i(TAG, "device reports a changed GATT database; rediscovering")
                documents.clear()
                g.discoverServices()
            }
        }

        @Deprecated("Kept for API levels below 33, which never call the byte[] overload.")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            @Suppress("DEPRECATION")
            val value = characteristic.value ?: ByteArray(0)
            handleRead(g, characteristic.uuid, value, status)
        }

        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            handleRead(g, characteristic.uuid, value, status)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            handler.post {
                if (g !== gatt) return@post
                val op = pending
                if (needsBonding(status) && op != null) {
                    deferUntilBonded(op)
                    opComplete()
                    return@post
                }
                if (status != BluetoothGatt.GATT_SUCCESS && op != null) {
                    failOp(op, "write rejected by device (status $status)")
                } else {
                    succeedOp(op)
                }
                opComplete()
            }
        }

        @Deprecated("Kept for API levels below 33, which never call the byte[] overload.")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            handleNotification(g, characteristic.uuid)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            handleNotification(g, characteristic.uuid)
        }
    }

    private fun handleRead(g: BluetoothGatt, uuid: UUID, value: ByteArray, status: Int) {
        handler.post {
            if (g !== gatt) return@post
            val op = pending
            if (needsBonding(status) && op != null) {
                deferUntilBonded(op)
                opComplete()
                return@post
            }
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.w(TAG, "read of $uuid failed: $status")
                opComplete()
                return@post
            }
            val json = value.toString(Charsets.UTF_8)
            when (uuid) {
                in REPORT_CHARS -> {
                    runCatching { mergeDocument(uuid, json) }
                        .onSuccess {
                            _telemetry.value = it
                            backoffMs = MIN_BACKOFF_MS
                            _connectionState.value = ConnectionState.READY
                        }
                        .onFailure { Log.w(TAG, "bad telemetry JSON from $uuid: $json", it) }
                }

                Protocol.STATUS_CHAR_UUID -> {
                    runCatching { CommandStatus.parse(json) }
                        .onSuccess { deliverStatus(it) }
                        .onFailure { Log.w(TAG, "bad status JSON: $json", it) }
                }
            }
            opComplete()
        }
    }

    /**
     * Keeps the newest copy of each of the three documents and parses them as
     * one. Merging rather than parsing each on its own means a notification on
     * one characteristic does not discard what the others last said; the
     * documents share no keys, so the order they are merged in does not
     * matter. Throws if the result does not parse, which is handled by the
     * caller.
     */
    private fun mergeDocument(uuid: UUID, json: String): Telemetry {
        documents[uuid] = JSONObject(json)
        val merged = JSONObject()
        for (document in documents.values) {
            for (key in document.keys()) merged.put(key, document.get(key))
        }
        return Telemetry.parse(merged.toString())
    }

    /**
     * A notification only carries a one byte placeholder, so the real value has
     * to be fetched with a read; the read is not truncated by the ATT MTU.
     */
    private fun handleNotification(g: BluetoothGatt, uuid: UUID) {
        handler.post {
            if (g !== gatt) return@post
            when (uuid) {
                in REPORT_CHARS -> enqueue(Op.Read(uuid))
                Protocol.STATUS_CHAR_UUID -> enqueue(Op.Read(Protocol.STATUS_CHAR_UUID))
            }
        }
    }

    private fun deliverStatus(status: CommandStatus) {
        _statusEvents.tryEmit(status)
        pendingCommands.remove(status.cmd)?.forEach { it.complete(status) }
    }

    private fun failAllPendingCommands(reason: String) {
        val waiters = pendingCommands.values.flatten()
        pendingCommands.clear()
        waiters.forEach { it.completeExceptionally(CommandException(reason)) }
    }

    // ------------------------------------------------------------------ bonding

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
            val changed: BluetoothDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
            }
            if (changed == null || changed.address != device?.address) return

            when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE)) {
                BluetoothDevice.BOND_BONDED -> handler.post {
                    Log.i(TAG, "bonded with ${changed.address}")
                    _connectionState.value = ConnectionState.DISCOVERING
                    while (true) enqueue(awaitingBond.poll() ?: break)
                    drain()
                }

                BluetoothDevice.BOND_NONE -> handler.post {
                    if (awaitingBond.isNotEmpty()) {
                        awaitingBond.forEach { failOp(it, "pairing was rejected") }
                        awaitingBond.clear()
                        _lastError.value = "pairing with the device failed"
                        teardownGatt()
                        scheduleReconnect()
                    }
                }
            }
        }
    }

    private fun registerBondReceiver() {
        if (bondReceiverRegistered) return
        ContextCompat.registerReceiver(
            appContext,
            bondReceiver,
            IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        bondReceiverRegistered = true
    }

    private fun unregisterBondReceiver() {
        if (!bondReceiverRegistered) return
        runCatching { appContext.unregisterReceiver(bondReceiver) }
        bondReceiverRegistered = false
    }

    // -------------------------------------------------------------- permissions

    private fun granted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(appContext, permission) == PackageManager.PERMISSION_GRANTED

    private fun hasConnectPermission(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            granted(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            true
        }

    private fun hasScanPermission(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            granted(Manifest.permission.BLUETOOTH_SCAN)
        } else {
            granted(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    companion object {
        /**
         * The characteristics the device reports through. They are read and
         * merged into one [Telemetry]; see [Protocol] for why there are three.
         */
        private val REPORT_CHARS = listOf(
            Protocol.TELEMETRY_CHAR_UUID,
            Protocol.SENSORS_CHAR_UUID,
            Protocol.CONFIG_CHAR_UUID,
        )

        private const val TAG = "TemplogBle"

        private const val MIN_BACKOFF_MS = 1_000L
        private const val MAX_BACKOFF_MS = 30_000L
        private const val SCAN_TIMEOUT_MS = 10_000L
        private const val DEFAULT_MTU = 23

        /** ATT opcode plus attribute handle, which every write carries. */
        private const val ATT_WRITE_OVERHEAD = 3
        private const val MIN_CHUNK_PAYLOAD = 16
        private const val CHUNK_TIMEOUT_MS = 10_000L
        const val COMMAND_TIMEOUT_MS = 10_000L

        /** The firmware asks for 247; requesting the maximum lets it win. */
        private const val PREFERRED_MTU = 517

        /* Spelled out rather than taken from BluetoothGatt, whose constant for
         * insufficient encryption only exists from API 29 onwards. */
        private const val GATT_INSUFFICIENT_AUTHENTICATION = 5
        private const val GATT_INSUFFICIENT_ENCRYPTION = 15
    }
}
