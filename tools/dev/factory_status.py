"""Parse one complete response from the existing S3 `hwtest status` command."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import re


class FactoryStatusError(ValueError):
    """A status response cannot be trusted."""


class IncompleteFactoryStatus(FactoryStatusError):
    """The complete status response has not arrived yet."""


@dataclass(frozen=True)
class FactoryStatus:
    active: bool
    stage: str
    step: str
    revision: int
    radio_role: str
    provisioning_state: str
    saved_radio: int | None
    save_pending: bool
    override_pending: bool
    durable_pass: bool
    override: bool
    current_failures: tuple[str, ...]
    checks: dict[str, str]
    observations: dict[str, str]


_MAX_RESPONSE_CHARS = 16 * 1024
_ANSI_CSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_UINT = r"[0-9]{1,10}"
_SAVED_TIMINGS = ("auto_ms", "operator_ms", "radio_ms")
_HEX_BYTE = r"0x[0-9a-fA-F]{2}"
_CHECK = r"(?:pass|pending|n/a)"
_LIGHT = r"(?:pass|pending|n/a|link_pending|hold_light_steady|change_light|uncover_in_room_light|cover_with_hand|uncover)"
_CHECK_NAMES = (
    "led_scan", "board_io", "light_1", "light_2", "motion_link", "motion_event", "led_review", "radio_ble",
)
_SENSOR = (
    rf"raw_lux=(?P<raw_lux>{_UINT}) room=(?P<room>{_UINT}) "
    rf"covered=(?P<covered>{_UINT}) recovered=(?P<recovered>{_UINT}) next=(?P<next>{_LIGHT})"
)
# The command prints these rows in order. Anchoring every row avoids joining
# a present result to stale fields from an earlier or interrupted response.
_ROWS = tuple((prefix, re.compile(pattern)) for prefix, pattern in (
    ("radio_role=", r"radio_role=(?P<radio_role>VERIFIER|DUT/testing) channel=(?P<channel>red|green|blue|white)"),
    ("active=", rf"active=(?P<active>[01]) stage=(?P<stage>[a-z_]+) step=(?P<step>[a-z_]+) revision=(?P<revision>{_UINT}) "
     rf"buttons=(?P<buttons>{_UINT}/{_UINT}) halls_clear=(?P<halls_clear>{_UINT})/64 "
     rf"halls_active=(?P<halls_active>{_UINT})/64"),
    ("button_mask=", r"button_mask=(?P<button_mask>0x[0-9a-fA-F]{4}/0x[0-9a-fA-F]{4}) "
     r"hall_clear=(?P<hall_clear>0x[0-9a-fA-F]{16}) hall_active=(?P<hall_active>0x[0-9a-fA-F]{16})"),
    ("responding_expanders=", rf"responding_expanders=(?P<responding_expanders>{_HEX_BYTE}) known=(?P<expander_links_known>[01]) "
     rf"motion_present=(?P<motion_present>[01]) known=(?P<motion_presence_known>[01]) "
     rf"forced_faults=(?P<forced_faults>{_HEX_BYTE}) current_failed_checks=(?P<current_failed_checks>{_HEX_BYTE})"),
    ("current_hall_read=", r"current_hall_read=(?P<current_hall_read>valid|unavailable) current_hall_clear=(?P<current_hall_clear>[01])"),
    ("sensor=1 ", "sensor=1 " + _SENSOR),
    ("sensor=2 ", "sensor=2 " + _SENSOR),
    ("auto_mask=", rf"auto_mask=(?P<auto_mask>{_HEX_BYTE}/{_HEX_BYTE}) auto_under_10s=(?P<auto_under_10s>[01]) "
     rf"motion_event=(?P<motion_event_observed>[01]) leds_confirmed=(?P<leds_confirmed>[01]) radio_mask=(?P<radio_mask>{_HEX_BYTE})"),
    ("checks ", "checks " + " ".join(
        rf"{key}=(?P<{key}>{_LIGHT if key in ('light_1', 'light_2') else _CHECK})" for key in _CHECK_NAMES
    )),
    ("radio_proof=", r"radio_proof=(?P<radio_proof>verified|pending_not_broken_radio_proof) peer_candidate=(?P<peer_candidate>[01]) "
     rf"discovery_deadline_ms=(?P<discovery_deadline_ms>{_UINT}) ble_endpoint_error=(?P<ble_endpoint_error>[01]) "
     r"save_pending=(?P<save_pending>[01]) override_pending=(?P<override_pending>[01])"),
    ("retained_radio_phase ", rf"retained_radio_phase current=(?P<retained_current>{_UINT}) previous=(?P<retained_previous>{_UINT}) "
     rf"previous_valid=(?P<retained_previous_valid>[01]) reset_reason=(?P<reset_reason>{_UINT})"),
    ("manual_ambient_response=", rf"manual_ambient_response=(?P<manual_ambient_response>required|not_required) observed_mask=(?P<ambient_observed_mask>{_HEX_BYTE}) "
     r"manual_motion=(?P<manual_motion>not_required) observed=(?P<manual_motion_observed>[01])"),
    ("provisioning_state=", r"provisioning_state=(?P<provisioning_state>required|absent|pending|passed|invalid|unavailable|legacy_exempt|override) "
     r"completed_result=(?P<completed_result>allowed|blocked) next=(?P<provisioning_next>[a-z_]+)"),
    ("saved_record=", rf"saved_record=(?P<saved_record>none|valid)(?: radio=(?P<saved_radio>[0-5]) verifier=(?P<verifier>[01]) "
     rf"auto_ms=(?P<auto_ms>{_UINT}) operator_ms=(?P<operator_ms>{_UINT}) radio_ms=(?P<radio_ms>{_UINT}))?"),
))


def saved_factory_result(fields: Mapping[str, str]) -> str | None:
    """Classify saved proof from a complete reply or a persisted result row."""
    verifier = fields.get("verifier")
    if (fields.get("saved_record") != "valid" or verifier not in {"0", "1"}
            or fields.get("save_pending") != "0" or fields.get("override_pending") != "0"
            or fields.get("revision") not in {"2", "3", "4"}):
        return None
    timings = [fields.get(key, "") for key in _SAVED_TIMINGS]
    if any(re.fullmatch(_UINT, value) is None or int(value) > 0xFFFFFFFF for value in timings):
        return None

    active, stage, step = (fields.get(key) for key in ("active", "stage", "step"))
    normal_boot = (active, stage, step) == ("0", "inactive", "led_field")
    saved_radio = fields.get("saved_radio")
    provisioning = fields.get("provisioning_state"), fields.get("completed_result")
    if provisioning == ("override", "blocked") and saved_radio == "5" and verifier == "0":
        if normal_boot or (active, stage, step) == ("0", "override_saved", "local_complete_waiting_for_peer"):
            return "override"
    if provisioning != ("passed", "allowed") or saved_radio not in {"1", "3"} or int(timings[0]) > 10000:
        return None
    if normal_boot:
        return "pass"
    if (stage, step) != ("passed", "pass_saved"):
        return None
    live_radio_proof = fields.get("radio_proof") == "verified" and fields.get("radio_mask") == "0x01"
    saved_verifier = fields.get("radio_role") == "VERIFIER" and verifier == "1"
    if (active == "1" and live_radio_proof) or (active == "0" and saved_verifier):
        return "pass"
    return None


def parse_factory_status(text: str) -> FactoryStatus:
    """Read one response. The caller owns USB identity and attempt freshness."""
    if len(text) > _MAX_RESPONSE_CHARS:
        raise FactoryStatusError("factory status response exceeded its size limit")

    expected = 0
    fields: dict[str, str] = {}
    for line in _ANSI_CSI.sub("", text).splitlines(keepends=True):
        # Even a valid-looking terminal number might still be arriving.
        if not line.endswith(("\r", "\n")):
            break
        body = line.strip().lstrip("> \t")
        if not body.startswith("hwtest: "):
            continue
        body = body.removeprefix("hwtest: ")
        for index, (prefix, pattern) in enumerate(_ROWS):
            if not body.startswith(prefix):
                continue
            if expected == 0 and index != 0:
                break  # Ignore residual output before this response begins.
            if index != expected:
                raise FactoryStatusError(f"out-of-order or duplicate factory status row: {prefix.strip()}")
            match = pattern.fullmatch(body)
            if match is None:
                raise FactoryStatusError(f"malformed factory status row: {prefix.strip()}")
            scope = f"sensor_{prefix[7]}_" if prefix.startswith("sensor=") else ""
            fields.update({scope + key: value for key, value in match.groupdict().items() if value is not None})
            expected += 1
            break
    if expected != len(_ROWS):
        raise IncompleteFactoryStatus(f"factory status awaiting {_ROWS[expected][0].strip()}")

    checks = {name: fields.pop(name) for name in _CHECK_NAMES}
    active = fields["active"] == "1"
    stage = fields["stage"]
    provisioning = fields["provisioning_state"]
    completed = fields["completed_result"] == "allowed"
    save_pending = fields["save_pending"] == "1"
    override_pending = fields["override_pending"] == "1"
    saved_radio = int(fields["saved_radio"]) if "saved_radio" in fields else None
    buttons_observed, buttons_required = (int(value) for value in fields["buttons"].split("/"))
    observed_mask, required_mask = (int(value, 16) for value in fields["button_mask"].split("/"))
    radio_mask = int(fields["radio_mask"], 16)
    if (
        completed != (provisioning in {"passed", "legacy_exempt"})
        or (fields["saved_record"] == "valid") != (saved_radio is not None)
        or (buttons_observed, buttons_required) != (bin(observed_mask).count("1"), bin(required_mask).count("1"))
        or observed_mask & ~required_mask
        or any(int(fields[f"halls_{state}"]) != bin(int(fields[f"hall_{state}"], 16)).count("1")
               for state in ("clear", "active"))
        or (fields["radio_proof"] == "verified") != (radio_mask == 1)
        or (checks["radio_ble"] == "pass") != bool(radio_mask & 1)
        or (checks["led_review"] == "pass") != (fields["leds_confirmed"] == "1")
        or fields["manual_motion_observed"] != fields["motion_event_observed"]
        or any(checks[f"light_{sensor}"] != fields[f"sensor_{sensor}_next"] for sensor in (1, 2))
    ):
        raise FactoryStatusError("factory status reports contradictory evidence")

    failures = []
    failed_mask = int(fields["current_failed_checks"], 16)
    for bit, label in enumerate(("LED drive failed", "Board I/O failed", "Light sensor link failed", "Motion sensor link failed")):
        if failed_mask & (1 << bit):
            failures.append(label)
    if failed_mask & ~0x0F:
        failures.append("Unknown factory check failed")
    if fields["ble_endpoint_error"] == "1":
        failures.append("Factory BLE endpoint failed to start")
    if provisioning in {"invalid", "unavailable"}:
        failures.append(f"Factory provisioning record {provisioning}")
    if stage == "failed" and not failures:
        failures.append("Current factory run failed")

    saved_result = saved_factory_result(fields)
    return FactoryStatus(
        active=active,
        stage=stage,
        step=fields["step"],
        revision=int(fields["revision"]),
        radio_role=fields["radio_role"],
        provisioning_state=provisioning,
        saved_radio=saved_radio,
        save_pending=save_pending,
        override_pending=override_pending,
        durable_pass=saved_result == "pass",
        override=saved_result == "override",
        current_failures=tuple(failures),
        checks=checks,
        observations=fields,
    )
