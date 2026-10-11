# The vault, and the one failure that isn't ours

The Suunto cloud session key lives in a Sailfish Secrets collection
(`src/secrets/tokenvault.cpp`), a near-direct port of OTP Cove's
`secretvault.cpp`. That file's own comments cover why the requests use
`SystemInteraction` rather than `PreventInteraction`, and they stay the
reference for that decision. This file covers something else: a failure mode
that looks exactly like an app bug, is not one, and cost an evening to pin
down on 2026-10-11.

## The symptom

Signing in reports

> Istunnon tallennus epäonnistui: Password Agent was unable to verify the
> authenticity of the user

and then every later operation that needs the session key — a health-data
upload, for instance — reports the same thing, repeatedly. The login itself
succeeds, because that is a plain HTTPS request; only the vault write fails,
so the key never survives to the next request.

Nothing appears in the journal. The message arrives in the D-Bus reply as
`Result::errorMessage()`; `sailfishsecretsd` does not log it.

## Where the message comes from

`plugins/passwordagentauthplugin/passwordagentplugin.cpp` in
[sailfishos/sailfish-secrets](https://github.com/sailfishos/sailfish-secrets).
The plugin asks polkit to `CheckAuthorization` the action
`org.sailfishos.secrets.authentication` for an `unix-process` subject built
from the caller's pid and the `start-time` it reads out of `/proc/<pid>/stat`,
then branches four ways on the reply:

| polkit reply | result |
|---|---|
| authorized | `Succeeded` |
| `polkit.dismissed` is true | `InteractionViewUserCanceledError` |
| not authorized, `details` **non-empty** | `IncorrectAuthenticationCodeError` — **this message** |
| not authorized, `details` empty | falls back to the device-lock API, with a comment that the caller "was not started by the lipstick user session so polkit cannot work for it" |

The action's own policy (`org.sailfishos.secrets.policy`) requires `auth_self`
for `allow_any`, `allow_inactive` and `allow_active` alike, so this path
always needs a real device-lock prompt. The prompt is shown by a polkit
authentication agent, and that agent lives in the lipstick session.

## The cause

A lipstick crash leaves the agent unregistered, and polkit then cannot satisfy
`auth_self` for anybody.

What was measured on the device while the symptom was live:

- `polkitd` — 0 restarts, running since 13:49:26, which is boot
- `sailfish-secretsd` — 0 restarts, running since 13:49:21, also boot
- `lipstick` — active since **20:38:14**

So neither daemon had died. Only lipstick had restarted, and it restarted
*after* polkitd, which is the whole asymmetry: polkitd still held the session's
agent registration from before the crash.

This also rules out the platform bug OTP Cove's comments warn about — the
`PasswordAgentPlugin::cancelAuthentication()` segfault. That one kills
`sailfishsecretsd`, and `sailfishsecretsd` had not restarted.

## The proof that it is not the app

OTP Cove, a different app on the same phone, could not show its codes either.
It uses the same polkit path, so the breakage was device-wide, not ours.

That check is worth doing *before* rebooting, because a reboot destroys the
distinction: afterwards there is no way to tell session residue from a latent
bug of our own. Here both apps recovered on reboot and the health-data upload
went through.

## What to do when this appears again

1. Open another Secrets-using app, OTP Cove for instance. If it is broken too,
   stop looking at this project.
2. Reboot. It restarts polkitd and lipstick together, so the agent
   re-registers.
3. If it survives a reboot, then it is worth suspecting this app, the device
   lock code, or the collection's own state — in that order.

A caveat on the whole chain: that lipstick crash was ours, from the
reqId-0 frame fixed in `75f7abc`. So an app that aborts under Wayland can take
down the compositor, and the dead compositor then breaks the device's
authentication for every other app until reboot. Worth remembering as the real
cost of letting an unexpected BLE frame reach an empty `std::function`.
