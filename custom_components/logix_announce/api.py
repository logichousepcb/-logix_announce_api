"""API client for Logix Message Announcer."""

from __future__ import annotations

from typing import Any

from aiohttp import ClientError, ClientSession

from .const import API_FILES, API_PLAY, API_STATUS, API_STOP, API_VERSION, API_VOLUME


class LogixAnnounceApiError(Exception):
    """Base API error."""


class LogixAnnounceApiConnectionError(LogixAnnounceApiError):
    """Raised when the device cannot be reached."""


class LogixAnnounceApiResponseError(LogixAnnounceApiError):
    """Raised when the device returns an invalid response."""


class LogixAnnounceApiClient:
    """HTTP client for the Logix Message Announcer device."""

    def __init__(self, session: ClientSession, host: str) -> None:
        self._session = session
        self._base_url = f"http://{host}"

    async def async_get_status(self) -> dict[str, Any]:
        """Fetch playback and queue status."""
        return await self._async_request("get", API_STATUS)

    async def async_get_files(self) -> dict[str, Any]:
        """Fetch file/playlist listing and queue settings."""
        return await self._async_request("get", API_FILES)

    async def async_get_version(self) -> dict[str, Any]:
        """Fetch firmware version."""
        return await self._async_request("get", API_VERSION)

    async def async_stop(self) -> dict[str, Any]:
        """Stop playback."""
        return await self._async_request("post", API_STOP)

    async def async_set_volume(self, volume: int) -> dict[str, Any]:
        """Set playback volume from 0 to 100."""
        return await self._async_request("post", API_VOLUME, json={"volume": volume})

    async def async_play_file(self, filename: str) -> dict[str, Any]:
        """Start playback for an SD file."""
        return await self._async_request("post", API_PLAY, json={"file": filename})

    async def async_play_url(self, url: str, loop: bool = False) -> dict[str, Any]:
        """Start playback for a URL stream."""
        return await self._async_request("post", API_PLAY, json={"url": url, "loop": loop})

    async def _async_request(
        self, method: str, path: str, json: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        """Perform an HTTP request and parse JSON response."""
        try:
            async with self._session.request(
                method, f"{self._base_url}{path}", json=json
            ) as response:
                if response.status >= 400:
                    message = await response.text()
                    raise LogixAnnounceApiResponseError(
                        f"API error {response.status}: {message}"
                    )

                data = await response.json()
        except ClientError as err:
            raise LogixAnnounceApiConnectionError(str(err)) from err
        except ValueError as err:
            raise LogixAnnounceApiResponseError("Invalid JSON response") from err

        if not isinstance(data, dict):
            raise LogixAnnounceApiResponseError("Expected JSON object response")

        if data.get("status") == "error":
            raise LogixAnnounceApiResponseError(str(data.get("message", "Unknown error")))

        return data
