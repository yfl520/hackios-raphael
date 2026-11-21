#!/var/jb/bin/sh
# Install only the explicit recorder UI. No camera, torch or capture auto-test.
set -e
app=/var/jb/Applications/HakiOSRecorder.app
/var/jb/usr/bin/mkdir -p /var/jb/Applications
# Updating the bundle while no recorder UI has been opened this boot is safe.
/var/jb/usr/bin/cp -R /redwood/apps/HakiOSRecorder.app /var/jb/Applications/
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12; do
 if /var/jb/usr/bin/uicache -p "$app" >/dev/null 2>&1; then exit 0; fi
 sleep 5
done
exit 1
