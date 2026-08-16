#include "wb_cmd.h"
#include "wb_ble.h"
#include "wb_control.h"
#include "wb_log.h"
#include "bapi.h"

namespace wb_cmd {

Plan buildCommand(const String& action, const String& value,
                  const String& owner, const String& metParam,
                  const String& parParam) {
    Plan p;

    // Idempotent start/stop (#23): skip a start that's already charging, or a
    // stop that's already stopped. Some chargers (e.g. Pulsar Plus USA fw) treat
    // a redundant w_cha as a TOGGLE and flip the wrong way; skipping also avoids
    // a needless BLE round-trip. Report success — already in the target state.
    if ((action == "start" || action == "stop") &&
        wallboxBLE.startStopRedundant(action == "start")) {
        Log.printf("[CMD] %s SKIPPED as redundant (isCharging=%d)\n",
                   action.c_str(), (int)wallboxBLE.isCharging());
        p.outcome     = Plan::RESPOND;
        p.statusCode  = 200;
        p.responseJson = "{\"status\":\"ok\",\"skipped\":\"already-in-target-state\"}";
        return p;
    }

    // Tag the commander for arbitration (advisory; see docs/control-owner.md).
    // Charge-affecting actions record who issued them (empty owner -> "manual")
    // so controllers can detect a recent manual/other override.
    if (action == "start" || action == "stop" ||
        action == "resume" || action == "current") {
        wb_control::recordCommand(owner);
    }

    p.outcome = Plan::ENQUEUE;
    if (action == "start") {
        p.met = bapi::MET_START_STOP;
        p.par = "1";
    }
    // w_cha stop par follows the charger's PRODUCT (chg_project), not the BLE
    // transport the auto-switch adopts: Pulsar Plus family -> par=0 (pause; a
    // Plus ACKs par=2 but ignores it, #4/#99), Pulsar MAX -> par=2 (hard stop).
    // A Plus on the Max single-char stack is still a Plus (par=0) — confirmed on
    // a prj08-pulsar-plus-pm3. isPlusCommandFamily() reads chg_project, falling
    // back to the configured model until fw_v_ is read.
    else if (action == "stop") {
        p.met = bapi::MET_START_STOP;
        p.par = wallboxBLE.isPlusCommandFamily() ? "0" : "2";
        Log.printf("[CMD] stop: chg_project='%s' inferred='%s' isPlusCmd=%d -> w_cha par=%s\n",
                   wallboxBLE.chargerProject().c_str(), wallboxBLE.inferredModel().c_str(),
                   (int)wallboxBLE.isPlusCommandFamily(), p.par.c_str());
    }
    // Resume clears the schedule/eco manual-override flag (r_dat.gen -> 0).
    // s_cmode mode=0 is rejected (subcode 6) ONLY while actively charging, so we
    // queue a defensive Stop first in that case alone. Sending the hard Stop
    // (par=2 on the MAX) when merely paused/waiting is NOT a harmless no-op — it
    // can fault the charger (error 114), so we skip it unless actually charging.
    else if (action == "resume") {
        if (wallboxBLE.isCharging()) {
            const char* stopPar = wallboxBLE.isPlusCommandFamily() ? "0" : "2";
            wallboxBLE.enqueueRequest(bapi::MET_START_STOP, stopPar);
        }
        p.met = "s_cmode";
        p.par = "{\"mode\":0}";
    }
    else if (action == "lock") {
        p.met = bapi::MET_LOCK;
        p.par = "1";
    }
    else if (action == "unlock") {
        p.met = bapi::MET_LOCK;
        p.par = "0";
    }
    // Clamp to the charger's own envelope (6 A .. charger-reported ceiling, #39).
    // Hard-coding 32 A capped 40 A USA Pulsar Plus units; maxCurrentCeiling()
    // falls back to 32 A when the charger doesn't report one. toInt()==0 on
    // garbage -> safe 6 A floor.
    else if (action == "current") {
        int amps = value.toInt();
        int hi = wallboxBLE.maxCurrentCeiling();
        if (amps < 6)  amps = 6;
        if (amps > hi) amps = hi;
        p.met = bapi::MET_SET_CURRENT;
        p.par = String(amps);
    }
    else if (action == "reboot") {
        p.met = bapi::MET_REBOOT;
        p.par = "null";
    }
    // Raw BAPI passthrough. Unlike the old sync copy (which forwarded an empty
    // met straight to the charger), a missing met is now a clean 400 — matching
    // what the async copy already did.
    else if (action == "bapi") {
        if (metParam.isEmpty()) {
            p.outcome     = Plan::RESPOND;
            p.statusCode  = 400;
            p.responseJson = "{\"error\":\"missing met\"}";
            return p;
        }
        p.met = metParam;
        p.par = parParam.isEmpty() ? String("null") : parParam;
    }
    else {
        p.outcome     = Plan::RESPOND;
        p.statusCode  = 400;
        p.responseJson = "{\"error\":\"unknown action\"}";
    }
    return p;
}

}  // namespace wb_cmd
