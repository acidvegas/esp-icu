# ESP-ICU

ESP-ICU *("I see you")* is tiny home security camera firmware for the Seeed XIAO ESP32-S3 Sense. Live view, recording, motion detection and clip playback all run on the camera and are served to any browser on your network.

## Features

- Live MJPEG view in the browser at the camera's full frame rate *(about 27 fps at 640x480)*
- Snapshots saved to the SD card next to recordings
- Manual record and stop
- Motion triggered recording with pre-roll, post-roll, adjustable sensitivity and drawable detection zones
- Audio from the Sense board's microphone recorded into clips and played back in the browser
- Browser playback of recorded clips with pause, seek and 0.5x to 8x speed
- Storage tab to play, view, download and delete clips and snapshots, one at a time or select all
- Push notifications through [ntfy](https://ntfy.sh) when motion starts, with a snapshot attached
- Keeps recordings for a set number of days and deletes the oldest clips when the card gets low on space
- Recording state, motion, SD card usage, fps, Wi-Fi signal and chip temperature at a glance
- View rotation for sideways mounts
- Image controls *(brightness, contrast, saturation, exposure, white balance, night mode)*
- Recovers on its own from a failed or reseated SD card and from a stalled camera
- HTTP basic auth on every page and API call

## Hardware

| Part                      | Notes                                                       |
|---------------------------|-------------------------------------------------------------|
| Seeed XIAO ESP32-S3 Sense | ESP32-S3R8, 8MB octal PSRAM, 8MB flash                      |
| Camera                    | OV3660 or OV2640 on the Sense expansion board               |
| microSD card              | FAT32, any size the card slot takes *(tested with 32GB)*    |
| Antenna                   | The U.FL antenna must be attached or Wi-Fi will not connect |

The Wi-Fi network must be 2.4 GHz. Hidden SSIDs work.

## Building

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.5 or newer.

```sh
cp main/config.h.example main/config.h   # then fill in Wi-Fi and web login
. ~/.local/share/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The camera, JPEG decoder and mDNS components are pulled in by the IDF component manager on the first build.

Once it connects, the serial log prints the address. Open `http://espicu.local/` or the IP directly.

## Configuration

`main/config.h` holds the things that should never be in the web UI:

| Define       | Purpose                                                       |
|--------------|---------------------------------------------------------------|
| `WIFI_SSID`  | Network name                                                  |
| `WIFI_PASS`  | Network password                                              |
| `WEB_USER`   | Login for the web page                                        |
| `WEB_PASS`   | Password for the web page                                     |
| `HOSTNAME`   | mDNS name *(`espicu` gives `espicu.local`)*                   |
| `NTFY_URL`   | ntfy server                                                   |
| `NTFY_TOPIC` | Topic to publish to                                           |
| `NTFY_TOKEN` | Access token with write access to the topic, empty to disable |

Everything else is set from the Settings panel in the web page and saved to flash:

| Setting          | Default | Notes                                                         |
|------------------|---------|---------------------------------------------------------------|
| Record on motion | off     | Turn on once the camera is mounted where you want it          |
| Sensitivity      | 60      | Higher catches smaller changes                                |
| Min motion size  | 2       | Zone cells *(16x12 grid)* that must change to count as motion |
| Pre-roll         | 3s      | Footage kept from before motion started                       |
| Post-roll        | 10s     | Keeps recording this long after motion stops                  |
| Max clip length  | 5 min   | Longer recordings are split into multiple clips, also at 1 GB |
| Keep recordings  | 14 days | 0 keeps everything until the card fills                       |
| Notify on motion | off     | Needs `NTFY_TOKEN`                                            |
| Cooldown         | 60s     | Minimum time between notifications                            |
| Record FPS       | 20      |                                                               |
| Resolution       | 640x480 | Up to 1600x1200                                               |
| JPEG quality     | 12      | Lower is better quality and bigger files                      |
| Rotate view      | 0       | Rotates the live view and playback in the browser only        |
| Image controls   | 0       | Brightness, contrast, saturation and exposure, -2 to +2       |
| White balance    | Auto    | Auto, sunny, cloudy, office or home                           |
| Night mode       | off     | Raises the gain ceiling for low light                         |
| Record audio     | on      | 16 kHz mono PCM track in each clip                            |
| Mic gain         | 4       | Digital gain, 1 to 16                                         |
| Timezone         | UTC0    | POSIX TZ string, e.g. `EST5EDT,M3.2.0,M11.1.0`                |
| Wi-Fi TX power   | 15 dBm  | 8 to 20, see the notes below                                  |

## Storage

Clips are MJPEG AVI files, with a PCM audio track when audio is on, that play in VLC and most desktop players. Snapshots are plain JPEGs:

```
/rec/YYYYMMDD/HHMMSS_m.avi    motion triggered clip
/rec/YYYYMMDD/HHMMSS_r.avi    manual clip
/snap/YYYYMMDD/HHMMSS.jpg     snapshot
```

If the card fails or is pulled, the camera unmounts it once nothing has a file open, shows the card as missing, and tries to mount it again every 30 seconds. A file that is playing or downloading cannot be deleted until it is closed.

The retention and low space cleanups cover both. Files saved before the clock syncs over NTP land in `19700101` folders. The retention cleanup leaves those alone; the low space cleanup removes them first since they sort oldest.

## Notifications

When motion starts and "Notify on motion" is on, the camera sends one push to `NTFY_URL/NTFY_TOPIC` with the current frame attached as a JPEG. Tapping the notification opens the web page. Only the start of a motion event triggers a push, and never more often than the cooldown.

The ntfy server needs attachments enabled *(`attachment-cache-dir` in its `server.yml`)* for the picture to come through.

## HTTP API

All endpoints need basic auth. Endpoints that change state also need an `X-Req` header so other websites cannot trigger them from your browser.

| Method | Path                        | Purpose                                      |
|--------|-----------------------------|----------------------------------------------|
| GET    | `/`                         | Web UI                                       |
| GET    | `/stream`                   | Live MJPEG stream                            |
| GET    | `/snapshot`                 | Current frame as JPEG                        |
| POST   | `/api/snapshot`             | Save the current frame to the SD card        |
| GET    | `/api/status`               | Recording, motion, fps, Wi-Fi, temp, SD      |
| GET    | `/api/settings`             | Current settings                             |
| POST   | `/api/settings`             | Update settings *(form encoded, any subset)* |
| POST   | `/api/record?on=1`          | Start *(1)* or stop *(0)* recording          |
| GET    | `/api/days`                 | Days that have clips or snapshots            |
| GET    | `/api/files?d=YYYYMMDD`     | Clips and snapshots for a day                |
| GET    | `/api/image?d=&f=`          | A saved snapshot                             |
| GET    | `/api/clip?d=&f=`           | Clip frame count, fps and size               |
| GET    | `/api/play?d=&f=&n=&speed=` | Clip as an MJPEG stream paced to real time   |
| GET    | `/api/frame?d=&f=&n=`       | Single frame of a clip                       |
| GET    | `/api/audio?d=&f=&n=`       | Clip audio as WAV, from frame `n`            |
| GET    | `/api/download?d=&f=`       | Download a clip or snapshot                  |
| POST   | `/api/delete?d=&f=`         | Delete a clip or snapshot                    |

Up to 4 streams and downloads can run at once.

## Notes

- **Wi-Fi TX power defaults to 15 dBm.** With the stock flat antenna, transmitting at 20 dBm dropped upload throughput to 20 to 34 KB/s, while anything at or below 18 dBm ran at 525 to 667 KB/s. Other antennas may behave differently, so the cap is a setting. 20 MHz channel width also beat 40 MHz.
- **The user LED cannot be used.** It shares GPIO21 with the SD card's DAT3 line, which has to stay high.
- **The SD card runs in SDMMC 1-bit mode at 40 MHz** on GPIO 7, 9 and 8. If a card was last used in SPI mode by other firmware, it stays in SPI mode until it loses power, so unplug the board once after flashing.
- **SD transfers go through internal RAM.** The S3 cannot DMA between the SD card and PSRAM, so recording and playback stage data in small internal buffers and write in whole blocks.
- The board runs warm with the camera on all the time. Give the case some airflow.

## Case

`case/` holds a minimal two-piece printed case for the board with the camera and the expansion board attached. The lens sits flush with a flat front, so the case can rest against a window. It also has a cutout for heatsinks on the back, a snap-fit lid and a panel hole for a U.FL to SMA pigtail.

| Path                 | Purpose                                                    |
|----------------------|------------------------------------------------------------|
| `case/case.scad`     | OpenSCAD model, `part` selects tray, lid or print plate    |
| `case/check.py`      | Renders the parts and checks fit against the Seeed STEP    |
| `case/print/`        | Sliced plate for a Bambu A1 Mini *(0.12 mm, PLA)*          |
| `case/index.html`    | 3D preview, served by `case/preview/serve.py`              |
| `case/ref/`          | Seeed STEP, KiCad and DXF sources the model is built from  |

Take the microSD card out before sliding the lid on. The reset button is not reachable once the case is closed.

## License

[ISC](LICENSE)
