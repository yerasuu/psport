/* The quirk registry's entries (docs/en/QUIRKS.md), expanded by runtime_quirks.h and
 * runtime_quirks.c, which define each macro before including this file:
 *
 *   QUIRK(id, kind, sz_reason, sz_reference)
 *       kind: LAUNCH, OPTIMIZATION, HOST or WORKAROUND; reason and reference are non-empty string
 *       literals (checked at compile time): why the quirk exists and the task, issue or decision
 *       that tracks it.
 *   QUIRK_GPU(id, vendor, driver_id, sz_device, version_first, version_last)
 *       a HOST quirk's match on the Vulkan device: PCI vendor id and VkDriverId (0: any), a
 *       substring of the device name ("": any) and an inclusive driverVersion range.
 *   QUIRK_TRAITS(id, traits)
 *       a match on the loader's middleware traits: every bit of the mask present.
 *
 * A title names its LAUNCH, OPTIMIZATION and WORKAROUND quirks in its Game::Title (quirks). */
