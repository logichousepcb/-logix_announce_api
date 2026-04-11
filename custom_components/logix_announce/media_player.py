"""Media player platform for Logix Message Announcer."""

from __future__ import annotations

from typing import Any
from urllib.parse import urljoin

import voluptuous as vol

from homeassistant.components import media_source
from homeassistant.components.media_player import (
    MediaPlayerEntity,
    MediaPlayerEntityFeature,
    async_process_play_media_url,
)
from homeassistant.components.media_player.const import MediaPlayerState, MediaType
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.network import NoURLAvailableError, get_url
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import entity_platform
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN, MANUFACTURER, MODEL
from .coordinator import LogixAnnounceDataUpdateCoordinator

SUPPORTED_FILE_EXTENSIONS = (".mp3", ".wav", ".m3u", ".m3u8")


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up the media player entity."""
    data = hass.data[DOMAIN][entry.entry_id]
    platform = entity_platform.async_get_current_platform()
    platform.async_register_entity_service(
        "play_url",
        {
            vol.Required("url"): cv.string,
            vol.Optional("loop", default=False): cv.boolean,
        },
        "async_play_url_service",
    )
    platform.async_register_entity_service(
        "play_file",
        {
            vol.Required("file"): cv.string,
        },
        "async_play_file_service",
    )
    async_add_entities([LogixAnnounceMediaPlayer(entry, data["coordinator"], data["api"])])


class LogixAnnounceMediaPlayer(CoordinatorEntity[LogixAnnounceDataUpdateCoordinator], MediaPlayerEntity):
    """Representation of a Logix Message Announcer media player."""

    _attr_has_entity_name = True
    _attr_name = "Player"
    _attr_supported_features = (
        MediaPlayerEntityFeature.PLAY_MEDIA
        | MediaPlayerEntityFeature.STOP
        | MediaPlayerEntityFeature.VOLUME_SET
        | MediaPlayerEntityFeature.SELECT_SOURCE
    )
    _attr_media_content_type = MediaType.MUSIC

    def __init__(
        self,
        entry: ConfigEntry,
        coordinator: LogixAnnounceDataUpdateCoordinator,
        api,
    ) -> None:
        """Initialize the media player."""
        super().__init__(coordinator)
        self._entry = entry
        self._api = api
        self._attr_unique_id = f"{entry.entry_id}_player"

    @property
    def device_info(self) -> dict[str, Any]:
        """Return device metadata."""
        return {
            "identifiers": {(DOMAIN, self._entry.entry_id)},
            "name": f"Logix Announcer ({self._entry.data[CONF_HOST]})",
            "manufacturer": MANUFACTURER,
            "model": MODEL,
            "configuration_url": f"http://{self._entry.data[CONF_HOST]}",
        }

    @property
    def state(self) -> MediaPlayerState:
        """Return current playback state."""
        status = self._status
        if status.get("playing"):
            return MediaPlayerState.PLAYING
        return MediaPlayerState.IDLE

    @property
    def media_title(self) -> str | None:
        """Return current file/stream label."""
        label = self._status.get("audio_file_label")
        if not label or label == "---":
            return None
        return str(label)

    @property
    def volume_level(self) -> float | None:
        """Return volume as 0.0..1.0."""
        volume = self._status.get("volume")
        if volume is None:
            return None
        return max(0.0, min(1.0, float(volume) / 100.0))

    @property
    def source(self) -> str | None:
        """Return currently selected source when it matches available files."""
        current = self.media_title
        if current and current in self.source_list:
            return current
        return None

    @property
    def source_list(self) -> list[str]:
        """Return audio file list from the device."""
        files = self._files.get("files", [])
        names: list[str] = []
        for item in files:
            if not isinstance(item, dict):
                continue
            if item.get("is_playlist"):
                continue
            name = item.get("name")
            if isinstance(name, str):
                names.append(name)
        return names

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        """Return extra attributes for diagnostics and automations."""
        status = self._status
        files = self._files
        return {
            "audio_state": status.get("audio_state"),
            "queue_enabled": status.get("queue_enabled"),
            "active_count": status.get("active_count"),
            "queue_pause_seconds": status.get("queue_pause_seconds"),
            "url_loop_enabled": status.get("url_loop_enabled"),
            "url_loop_armed": status.get("url_loop_armed"),
            "file_count": files.get("count"),
        }

    @property
    def _status(self) -> dict[str, Any]:
        data = self.coordinator.data or {}
        return data.get("status", {})

    @property
    def _files(self) -> dict[str, Any]:
        data = self.coordinator.data or {}
        return data.get("files", {})

    async def async_set_volume_level(self, volume: float) -> None:
        """Set output volume."""
        level = int(max(0, min(100, round(volume * 100))))
        await self._api.async_set_volume(level)
        await self.coordinator.async_request_refresh()

    async def async_media_stop(self) -> None:
        """Stop playback."""
        await self._api.async_stop()
        await self.coordinator.async_request_refresh()

    async def async_play_media(
        self,
        media_type: str | MediaType,
        media_id: str,
        **kwargs: Any,
    ) -> None:
        """Play either a URL stream or SD file by media_id."""
        loop = bool(kwargs.get("extra", {}).get("loop", False))

        processed_media_id = media_id

        if processed_media_id.startswith("media-source://"):
            resolved = await media_source.async_resolve_media(self.hass, processed_media_id)
            processed_media_id = resolved.url

        processed_media_id = async_process_play_media_url(self.hass, processed_media_id)

        if processed_media_id.startswith("http://") or processed_media_id.startswith("https://"):
            await self._api.async_play_url(processed_media_id, loop=loop)
        elif processed_media_id.startswith("/"):
            try:
                base_url = get_url(self.hass, prefer_external=False)
            except NoURLAvailableError as err:
                raise HomeAssistantError(
                    "No internal Home Assistant URL is configured for TTS/media playback"
                ) from err

            await self._api.async_play_url(urljoin(base_url, processed_media_id), loop=loop)
        elif "/" in processed_media_id and processed_media_id.lower().startswith(("api/", "local/", "media/")):
            try:
                base_url = get_url(self.hass, prefer_external=False)
            except NoURLAvailableError as err:
                raise HomeAssistantError(
                    "No internal Home Assistant URL is configured for TTS/media playback"
                ) from err

            await self._api.async_play_url(urljoin(base_url + "/", processed_media_id), loop=loop)
        elif processed_media_id.lower().endswith(SUPPORTED_FILE_EXTENSIONS):
            await self._api.async_play_file(processed_media_id)
        else:
            try:
                base_url = get_url(self.hass, prefer_external=False)
            except NoURLAvailableError as err:
                raise HomeAssistantError(
                    "No internal Home Assistant URL is configured for TTS/media playback"
                ) from err

            await self._api.async_play_url(urljoin(base_url + "/", processed_media_id), loop=loop)
        await self.coordinator.async_request_refresh()

    async def async_select_source(self, source: str) -> None:
        """Play a source from source_list."""
        await self._api.async_play_file(source)
        await self.coordinator.async_request_refresh()

    async def async_play_url_service(self, url: str, loop: bool = False) -> None:
        """Handle integration service: play_url."""
        await self._api.async_play_url(url, loop=loop)
        await self.coordinator.async_request_refresh()

    async def async_play_file_service(self, file: str) -> None:
        """Handle integration service: play_file."""
        await self._api.async_play_file(file)
        await self.coordinator.async_request_refresh()
