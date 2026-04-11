"""Data coordinator for Logix Message Announcer."""

from __future__ import annotations

import logging
from datetime import timedelta
from typing import Any

from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import LogixAnnounceApiClient, LogixAnnounceApiError
from .const import DEFAULT_SCAN_INTERVAL, DOMAIN

_LOGGER = logging.getLogger(__name__)


class LogixAnnounceDataUpdateCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Coordinator to fetch device state."""

    def __init__(
        self,
        hass: HomeAssistant,
        api: LogixAnnounceApiClient,
        scan_interval: int = DEFAULT_SCAN_INTERVAL,
    ) -> None:
        """Initialize the coordinator."""
        super().__init__(
            hass,
            logger=_LOGGER,
            name=DOMAIN,
            update_interval=timedelta(seconds=scan_interval),
        )
        self.api = api

    async def _async_update_data(self) -> dict[str, Any]:
        """Fetch data from the device."""
        try:
            status = await self.api.async_get_status()
            files = await self.api.async_get_files()
        except LogixAnnounceApiError as err:
            raise UpdateFailed(str(err)) from err

        return {
            "status": status,
            "files": files,
        }
