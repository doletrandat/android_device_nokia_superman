# BV-T5A percentage investigation

`ocv_estimate.py` interpolates the recovered stock BV-T5A OCV table at the
measured battery temperature. It refuses samples above 20mA absolute battery
current, out-of-range measurements and malformed calibration columns. The
original 70C column has 5%=3183mV followed by 4%=3622mV; that column and any
interpolation requiring it are rejected, without correcting the original data.

This is a diagnostic voltage-curve estimate. This loaded-voltage diagnostic does not publish a power-supply property.
The companion driver in `../current` uses hardware OCV and calibrated coulomb
counts to publish Android capacity; loaded voltage is not substituted for OCV.
Ageing, rest recovery and full-cycle accuracy remain unqualified.

The private recovered `DECODED-PROFILE.json` records original provision SHA-256
`7b7622decd9ea01da82bfea6fd5c83ce74b25c9b507f06f191196f3c7e7d7ae9`.
Its provenance field is checked, not treated as authentication of arbitrary input.
Original firmware/provision payloads remain private. The current module contains
the five valid decoded OCV columns needed for battery reporting.

`bms_math.py` converts a coherent PM8226 BMS1 open-circuit-voltage/coulomb-counter
sample using measured VADC references, IADC gain/offset and sense resistance.
It uses the same OCV interpolation and subtracts hardware-counted consumed charge
from nominal profile capacity. Positive BMS counter values mean discharge. Sample
and calibration boot IDs must match. It never substitutes loaded battery voltage
for the hardware OCV input or writes a power-supply property.

On boot `6beaf763-6c75-4dc1-aaa7-a813b4c55580`, 16 coherent samples over 21.305s
gave 1425uAh consumed; integrating the normal IADC current samples gave 1467.825uAh
(2.92% difference). This is an internal cross-check, without an external shunt or
full-cycle qualification. Measured calibration: gain51104, offset47919,
internal8859 micro-ohms; VADC625mV raw31166 and1250mV raw37529. The final sample
maps to89.78% under the nominal2220mAh profile. Learned capacity and full-cycle accuracy remain unqualified. PF37 integrates
the hardware OCV/CC model into Android; see `../current/README.md`. The user reports
that this replacement BV-T5A reports percentage correctly under Windows Phone.

Run `bms_math.py PROFILE.json SAMPLE.json CALIBRATION.json` for the diagnostic.
Acquired inputs and readbacks are under
`.codex-restart/artifacts/pf36-bms-gauge-20261103`; they are not included in source.

Run `python3 -m unittest discover -s device/nokia/superman/power/gauge` from the
Android source root. The diagnostic takes the profile path, microvolts, deci-C,
and signed microamps in that order. Qualification evidence is under
`.codex-restart/artifacts/pf35-power-20261103`.
