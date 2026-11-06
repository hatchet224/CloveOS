# R54 brightness lifecycle correction candidate

R53 hardware captures identified the source of the reversal: the shipped
backlight-disable interface changes PWM polarity from inverse to normal, and
reopening the backlight retains normal polarity. The static inverse brightness
adapter therefore worked before sleep and ran backwards after it. See the
[sanitized diagnostic samples](../r53/HARDWARE_TRANSITION.json).

This candidate uses the existing display interface to normalize the mode before
the first brightness restoration. Boot and wake then use the same positive
mapping: increasing the selected level raises both PWM brightness and the meter.
It stores a logical selection separately from transient driver values and
converts authenticated older settings to the new versioned format. The old
format did not record polarity, so migration preserves its logical meter choice;
exact pre-update physical light cannot be inferred from that old value alone.

The targeted software checks and reconstruction instructions are included.
Hardware-state observations remain available for device confirmation. Passing
models and cached register checks do not establish visible panel behavior.
The user confirmed the physical check passed: Menu + Volume Up/Down and
brightness retention through the requested two sleep/wake cycles worked as
intended. See [acceptance record](PHYSICAL_ACCEPTANCE.json). This is user-reported
acceptance; post-correction PWM logs and optical measurements are not claimed.
No public R54 binary release is claimed by this source record.

The correction is limited to brightness state, initialization, storage and
associated tests. The queued Ports Collection layout remains separate. Existing
third-party notices and vendor-source review are unchanged.
