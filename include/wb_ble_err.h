#pragma once
#include <Arduino.h>

// NimBLE error-code decoder (#161).
//
// The BLE log lines print raw NimBLE return codes as bare numbers — a
// disconnect reason of 531, a GATT err of 261 — which nobody can read without
// digging through the library headers. NimBLE packs the code's *class* into the
// high byte (see host/ble_hs.h): 0x100+ = ATT/GATT reply, 0x200+ = HCI reason
// (what event->disconnect.reason carries), 0x400/0x500+ = SMP pairing; anything
// below 0x100 is a plain ble_hs host error (1..31). bleErrName() splits on that
// range and returns a short human string for the codes that actually turn up on
// our connect / pair / encrypt / disconnect paths, plus the common ones
// (auth / timeout / remote-terminated / conn-limit). Unknown codes return "" so
// the caller still prints the number and nothing is lost.
//
// Compact lookup only — deliberately not every code. Kept header-only/inline so
// any module logging a NimBLE code can reuse it without a link dependency.

// HCI-layer reason (event->disconnect.reason, encoded as 0x200 + code).
// Definitive names from nimble/ble.h (BLE_ERR_*). Covers the ones seen on air.
static inline const char* bleHciReasonName(int hci) {
    switch (hci) {
        case 0x05: return "auth failure";
        case 0x06: return "PIN/key missing";
        case 0x08: return "supervision timeout (link lost)";
        case 0x09: return "connection limit exceeded";
        case 0x0c: return "command disallowed";
        case 0x0d: return "rejected — no resources";
        case 0x0e: return "rejected — security";
        case 0x10: return "connection accept timeout";
        case 0x13: return "remote terminated connection";
        case 0x14: return "remote terminated — low resources";
        case 0x15: return "remote terminated — power off";
        case 0x16: return "connection terminated locally";
        case 0x17: return "repeated pairing attempts";
        case 0x18: return "pairing not allowed";
        case 0x1f: return "unspecified error";
        case 0x22: return "LMP/LL response timeout";
        case 0x3d: return "terminated — MIC failure";
        case 0x3e: return "connection establishment failed";
        default:   return "";
    }
}

// ATT/GATT reply status (encoded as 0x100 + code). Definitive names from
// host/ble_att.h (BLE_ATT_ERR_*). These are the read/write rejections.
static inline const char* bleAttErrName(int att) {
    switch (att) {
        case 0x01: return "invalid handle";
        case 0x02: return "read not permitted";
        case 0x03: return "write not permitted";
        case 0x05: return "insufficient authentication";
        case 0x06: return "request not supported";
        case 0x08: return "insufficient authorisation";
        case 0x0a: return "attribute not found";
        case 0x0c: return "insufficient key size";
        case 0x0d: return "invalid attribute value length";
        case 0x0f: return "insufficient encryption";
        case 0x11: return "insufficient resources";
        default:   return "";
    }
}

// Plain ble_hs host error (1..31, from host/ble_hs.h BLE_HS_E*). These are the
// codes getLastError() usually returns on our connect / secureConnection path.
static inline const char* bleHsErrName(int hs) {
    switch (hs) {
        case 3:  return "EINVAL (bad argument)";
        case 4:  return "EMSGSIZE (bad length)";
        case 6:  return "ENOMEM (out of memory)";
        case 7:  return "ENOTCONN (link dropped mid-op)";
        case 8:  return "ENOTSUP (unsupported)";
        case 12: return "ECONTROLLER (controller error)";
        case 13: return "ETIMEOUT (timed out)";
        case 15: return "EBUSY (stack busy)";
        case 16: return "EREJECT (peer rejected)";
        case 17: return "EUNKNOWN (unexpected)";
        case 19: return "ETIMEOUT_HCI (controller timeout)";
        case 22: return "ENOTSYNCED (stack not ready)";
        case 23: return "EAUTHEN (auth/pairing failed)";
        case 24: return "EAUTHOR (not authorised)";
        case 25: return "EENCRYPT (encryption failed)";
        case 26: return "EENCRYPT_KEY_SZ (key size)";
        default: return "";
    }
}

// Decode any NimBLE return code to a short name. Dispatches on the high byte so
// the same call handles a host error, a GATT reply and a disconnect reason.
// Returns "" for codes we don't map (caller still prints the number).
static inline const char* bleErrName(int e) {
    if (e == 0)                    return "OK";
    if (e >= 0x200 && e <= 0x2ff)  return bleHciReasonName(e - 0x200);  // HCI reason
    if (e >= 0x100 && e <= 0x1ff)  return bleAttErrName(e - 0x100);     // ATT/GATT
    if (e >= 0x400 && e <= 0x5ff)  return "SMP pairing failure";        // SM us/peer
    return bleHsErrName(e);                                             // host error
}
