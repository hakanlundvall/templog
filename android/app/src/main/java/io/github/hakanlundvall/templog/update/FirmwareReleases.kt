package io.github.hakanlundvall.templog.update

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/** A published firmware release that the device can install. */
data class FirmwareRelease(
    /** The release tag, which is also the version the firmware reports. */
    val tag: String,
    val publishedAt: String?,
    val htmlUrl: String?,
    /** Direct link to firmware.bin, used when the phone sends the image over BLE. */
    val assetUrl: String,
    val assetSize: Int,
)

class ReleaseCheckException(message: String) : IOException(message)

/**
 * Firmware releases on GitHub. Normally the app only needs to know which tag
 * is newest, because the device downloads the image over its own Wi-Fi; it
 * also fetches the image itself when it has to send it over BLE instead.
 */
object FirmwareReleases {
    private const val LATEST_URL =
        "https://api.github.com/repos/hakanlundvall/templog/releases/latest"

    /** Must match OTA_RELEASE_ASSET in the firmware (include/ota.h). */
    private const val ASSET_NAME = "firmware.bin"

    private const val TIMEOUT_MS = 15_000

    /** Guards against pulling something far too large into memory. */
    private const val MAX_IMAGE_BYTES = 4 * 1024 * 1024

    /** Returns the latest release, or null if none has been published yet. */
    suspend fun latest(): FirmwareRelease? = withContext(Dispatchers.IO) {
        val connection = URL(LATEST_URL).openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            connection.setRequestProperty("User-Agent", "templog-android")

            when (val code = connection.responseCode) {
                HttpURLConnection.HTTP_OK -> Unit
                HttpURLConnection.HTTP_NOT_FOUND -> return@withContext null
                else -> throw ReleaseCheckException("GitHub answered HTTP $code")
            }
            val body = connection.inputStream.bufferedReader().use { it.readText() }
            val root = JSONObject(body)
            val assets = root.optJSONArray("assets")
            val firmware = (0 until (assets?.length() ?: 0))
                .map { assets!!.getJSONObject(it) }
                .firstOrNull { it.optString("name") == ASSET_NAME }
                ?: throw ReleaseCheckException("latest release has no $ASSET_NAME")
            FirmwareRelease(
                tag = root.getString("tag_name"),
                publishedAt = root.optString("published_at").takeIf { it.isNotEmpty() },
                htmlUrl = root.optString("html_url").takeIf { it.isNotEmpty() },
                assetUrl = firmware.getString("browser_download_url"),
                assetSize = firmware.optInt("size"),
            )
        } finally {
            connection.disconnect()
        }
    }

    /**
     * Fetches the image itself, for pushing to a device that cannot reach
     * GitHub on its own. Reports progress as a fraction of the known asset
     * size so a slow mobile connection still shows movement.
     */
    suspend fun download(
        release: FirmwareRelease,
        onProgress: (bytes: Int, total: Int) -> Unit,
    ): ByteArray = withContext(Dispatchers.IO) {
        val connection = URL(release.assetUrl).openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.instanceFollowRedirects = true
            connection.setRequestProperty("User-Agent", "templog-android")

            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw ReleaseCheckException("GitHub answered HTTP ${connection.responseCode}")
            }
            val total = connection.contentLength.takeIf { it > 0 } ?: release.assetSize
            if (total > MAX_IMAGE_BYTES) {
                throw ReleaseCheckException("$ASSET_NAME is unexpectedly large ($total bytes)")
            }

            val out = ByteArrayOutputStream(total.coerceAtLeast(DEFAULT_BUFFER_SIZE))
            val buffer = ByteArray(16 * 1024)
            connection.inputStream.use { input ->
                while (true) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    out.write(buffer, 0, read)
                    if (out.size() > MAX_IMAGE_BYTES) {
                        throw ReleaseCheckException("$ASSET_NAME is unexpectedly large")
                    }
                    onProgress(out.size(), total)
                }
            }
            out.toByteArray()
        } finally {
            connection.disconnect()
        }
    }
}
