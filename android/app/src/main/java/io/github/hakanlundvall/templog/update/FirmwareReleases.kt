package io.github.hakanlundvall.templog.update

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/** A published firmware release that the device can install. */
data class FirmwareRelease(
    /** The release tag, which is also the version the firmware reports. */
    val tag: String,
    val publishedAt: String?,
    val htmlUrl: String?,
)

class ReleaseCheckException(message: String) : IOException(message)

/**
 * Looks up firmware releases on GitHub. The device downloads the image
 * itself; the app only needs to know which tag is newest, to offer it.
 */
object FirmwareReleases {
    private const val LATEST_URL =
        "https://api.github.com/repos/hakanlundvall/templog/releases/latest"

    /** Must match OTA_RELEASE_ASSET in the firmware (include/ota.h). */
    private const val ASSET_NAME = "firmware.bin"

    private const val TIMEOUT_MS = 15_000

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
            val hasFirmware = (0 until (assets?.length() ?: 0))
                .any { assets!!.getJSONObject(it).optString("name") == ASSET_NAME }
            if (!hasFirmware) {
                throw ReleaseCheckException("latest release has no $ASSET_NAME")
            }
            FirmwareRelease(
                tag = root.getString("tag_name"),
                publishedAt = root.optString("published_at").takeIf { it.isNotEmpty() },
                htmlUrl = root.optString("html_url").takeIf { it.isNotEmpty() },
            )
        } finally {
            connection.disconnect()
        }
    }
}
