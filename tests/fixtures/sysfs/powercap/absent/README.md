# absent

A sysfs root with no powercap and no power PMU: what the development
container's /sys is (`ls /sys/class/powercap` fails, and `/sys/bus/event_source/devices`
holds no `power` in the sandboxes that lack it). The directory exists so that a
source can be pointed at it; the README is the only file, because git holds no
empty directory.
