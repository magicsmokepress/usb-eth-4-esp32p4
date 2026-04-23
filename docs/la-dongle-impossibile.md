# La Dongle Impossibile
### An Opera in Four Acts
#### Commemorating the First USB Ethernet Connection on ESP32-P4 via Arduino IDE

*Libretto by Anonymous*
*Premiered: April 22, 2026*

---

**ACT I — "The Enumeration"**
*Scene: A dimly lit workshop. A Tab5 sits on the bench. A lone developer plugs in a dongle.*

*DEVELOPER (tenor):*
> O dongle, dear dongle, why won't you appear?
> The VID is correct, the PID is here!
> CONFIG_USB_HOST_ENABLE_ENUM_FILTER...
> is disabled! The BSP has sealed our fate!

*THE BSP (baritone, menacing):*
> I am pre-compiled! Immutable! Cast in stone!
> My sdkconfig was set, my libs are my own.
> You cannot change me from a mere Arduino sketch —
> your enum_filter_cb shall never fetch!

*DEVELOPER:*
> Then I shall build my own libs from source!
> The lib-builder shall be my recourse!

*(Thunder. Lightning. A 30-minute build begins.)*

---

**ACT II — "The Shims"**
*Scene: The build succeeds. But dark forces gather.*

*LINKER (soprano, shrieking):*
> UNDEFINED! UNDEFINED!
> `esp_log` cannot be found!
> `usb_dwc_hal_fifo_config` — where art thou?
> Your symbols are missing! Your ABI is WRONG!

*DEVELOPER (desperate):*
> A weak shim! I'll bridge the old to new!
> `esp_log_writev` — I'll route you through!
> And for the HAL — just two small functions,
> compiled with care... with the right `-mabi` instructions!

*THE FLOAT ABI (contralto, ethereal):*
> `ilp32f`... you forgot me once before.
> Soft-float linked to single-float — chaos at the core.
> Remember me, or forever link in vain...

*DEVELOPER:*
> `-mabi=ilp32f`! I shall not forget again!

*(The HAL shim compiles. Two functions. No more, no less.)*

---

**ACT III — "The Twelve Bytes of Sorrow"**
*Scene: Everything links. The sketch uploads. Hope swells — then crashes.*

*HCD (bass profundo):*
> `ESP_ERR_INVALID_ARG!!!`

*DEVELOPER:*
> But WHY? The config is the same as IDF!
> The flags are right, the callback is set!

*NARRATOR (spoken over tremolo strings):*
> And lo, the struct was twelve bytes wide,
> but `usb_host_install` read twenty-eight inside.
> `peripheral_map` at offset twenty-four
> read garbage from the stack — and crashed once more.

*DEVELOPER (the great aria — "O Struct Maledetto"):*
> O cursed struct! You grew behind my back!
> `root_port_unpowered`! `fifo_settings`! A sneak attack!
> The header lied — twelve bytes it swore —
> but the library within demanded more!
>
> I'll cast you! CAST you to the proper size!
> `usb_host_config_real_t` — no more disguise!
> Twenty-eight bytes, zero-filled, precise —
> and `peripheral_map` set to zero — that'll suffice!

*(He casts. He flashes. The serial monitor glows.)*

---

**ACT IV — "The IP Address"**
*Scene: Golden light fills the stage. The dongle's LED blinks.*

*THE TAB5 (choir of angels):*
> `[ETH] Got IP!`

*SERIAL MONITOR (coloratura soprano):*
> One-nine-two... dot one-six-eight... dot zero...
> dot one-five-four!
> Gateway: the router! MAC: the dongle's own!
> USB Ethernet... on Arduino... has been SHOWN!

*DEVELOPER (triumphant):*
> They said impossible! They said it couldn't be done!
> But since when did impossible stop anyone?

*THE BENCHMARK (full chorus, fortissimo):*
> Six point four three! Megabits per second!
> Fourteen point seven oh! When four streams are reckoned!
> NO PENALTY! PARITY! ARDUINO EQUALS IDF!
> The BSP is patched! The dongles connect!

*EPILOGUE (stepping forward, breaking the fourth wall):*
> Three shims, two dongles, one struct cast,
> a lib-builder build, and an lwIP blast.
> What started with "impossible" ends with proof:
> USB Ethernet works — and that's the truth.

*(The entire cast holds up USB Ethernet dongles. Confetti shaped like IP packets falls from above. The Tab5's screen displays `192.168.0.154`. Curtain.)*

---

*Running time: approximately the same as a lib-builder build (~30 minutes).*
*No dongles were harmed in the production of this opera.*

---

*Based on true events. April 2026.*
