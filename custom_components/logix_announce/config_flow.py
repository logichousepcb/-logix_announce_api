"""Config flow for Logix Message Announcer."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import (
    LogixAnnounceApiClient,
    LogixAnnounceApiConnectionError,
    LogixAnnounceApiResponseError,
)
from .const import CONF_HOST, CONF_SCAN_INTERVAL, DEFAULT_SCAN_INTERVAL, DOMAIN


async def _validate_input(hass, data: dict[str, Any]) -> dict[str, Any]:
    """Validate user input by contacting the device."""
    api = LogixAnnounceApiClient(async_get_clientsession(hass), data[CONF_HOST])
    status = await api.async_get_status()
    version = await api.async_get_version()

    title = data[CONF_HOST]
    firmware = version.get("version")
    if firmware:
        title = f"{title} (v{firmware})"

    return {
        "title": title,
        "audio_state": status.get("audio_state", "unknown"),
    }


class LogixAnnounceConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for Logix Message Announcer."""

    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None):
        """Handle initial setup step."""
        errors: dict[str, str] = {}

        if user_input is not None:
            await self.async_set_unique_id(user_input[CONF_HOST])
            self._abort_if_unique_id_configured()

            try:
                info = await _validate_input(self.hass, user_input)
            except LogixAnnounceApiConnectionError:
                errors["base"] = "cannot_connect"
            except LogixAnnounceApiResponseError:
                errors["base"] = "invalid_response"
            except Exception:
                errors["base"] = "unknown"
            else:
                return self.async_create_entry(title=info["title"], data=user_input)

        schema = vol.Schema(
            {
                vol.Required(CONF_HOST): str,
                vol.Optional(CONF_SCAN_INTERVAL, default=DEFAULT_SCAN_INTERVAL): vol.All(
                    vol.Coerce(int), vol.Range(min=1, max=300)
                ),
            }
        )

        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)
