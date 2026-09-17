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

    /** Re-reads the telemetry characteristic without waiting for a notification. */
    fun refreshTelemetry() {
        handler.post {
            if (_connectionState.value == ConnectionState.READY) {
                enqueue(Op.Read(Protocol.TELEMETRY_CHAR_UUID))
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

    private fun characteristic(uuid: UUID): BluetoothGattCharacteristic? =
        gatt?.getService(Protocol.SERVICE_UUID)?.getCharacteristic(uuid)

    private fun subscribe(gatt: BluetoothGatt, uuid: UUID): Boolean {
        val characteristic = characteristic(uuid) ?: return false
        if (!gatt.setCharacteristicNotification(characteristic, true)) return false
        val cccd = characteristic.getDescriptor(Protocol.CCCD_UUID) ?: return false
        val enable = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(cccd, enable) == BluetoothGatt.GATT_SUCCESS
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
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(
                characteristic,
                op.value,
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
            ) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
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
                if (status != BluetoothGatt.GATT_SUCCESS || g.getService(Protocol.SERVICE_UUID) == null) {
                    _lastError.value = "templog GATT service not found on this device"
                    teardownGatt()
                    scheduleReconnect()
                    return@post
                }
                // Subscribing touches an encrypted characteristic, which is
                // what triggers pairing on a fresh device.
                enqueue(Op.Subscribe(Protocol.TELEMETRY_CHAR_UUID))
                enqueue(Op.Subscribe(Protocol.STATUS_CHAR_UUID))
                enqueue(Op.Read(Protocol.TELEMETRY_CHAR_UUID))
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
                Protocol.TELEMETRY_CHAR_UUID -> {
                    runCatching { Telemetry.parse(json) }
                        .onSuccess {
                            _telemetry.value = it
                            backoffMs = MIN_BACKOFF_MS
                            _connectionState.value = ConnectionState.READY
                        }
                        .onFailure { Log.w(TAG, "bad telemetry JSON: $json", it) }
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
     * A notification only carries a one byte placeholder, so the real value has
     * to be fetched with a read; the read is not truncated by the ATT MTU.
     */
    private fun handleNotification(g: BluetoothGatt, uuid: UUID) {
        handler.post {
            if (g !== gatt) return@post
            when (uuid) {
                Protocol.TELEMETRY_CHAR_UUID -> enqueue(Op.Read(Protocol.TELEMETRY_CHAR_UUID))
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
        private const val TAG = "TemplogBle"

        private const val MIN_BACKOFF_MS = 1_000L
        private const val MAX_BACKOFF_MS = 30_000L
        private const val SCAN_TIMEOUT_MS = 10_000L
        const val COMMAND_TIMEOUT_MS = 10_000L

        /** The firmware asks for 247; requesting the maximum lets it win. */
        private const val PREFERRED_MTU = 517

        /* Spelled out rather than taken from BluetoothGatt, whose constant for
         * insufficient encryption only exists from API 29 onwards. */
        private const val GATT_INSUFFICIENT_AUTHENTICATION = 5
        private const val GATT_INSUFFICIENT_ENCRYPTION = 15
    }
}
