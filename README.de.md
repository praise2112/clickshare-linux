# ClickShare unter Linux

Teile deinen Linux-Bildschirm mit einem **ClickShare Button** in einem
Besprechungsraum mit Barco ClickShare. Der Button ist der USB-Dongle auf dem
Tisch im Besprechungsraum. Barco bietet die ClickShare-App nur für Windows und
macOS an. Dieses Projekt führt die Windows-App unter Wine aus und ergänzt die
Bildschirmaufnahme, die Wine fehlt, sowohl unter Wayland als auch unter X11.

English version: [README.md](README.md)

| Funktion | Status |
|---|---|
| Kopplung des Buttons mit dem Raum | Funktioniert |
| Bildschirm teilen | Funktioniert unter Wayland (GNOME) und X11 |
| Einzelnes Fenster teilen | Funktioniert unter Wayland, über GNOMEs Auswahldialog |
| Einen separaten virtuellen Bildschirm teilen, den nur der Raumbildschirm zeigt | Funktioniert unter Wayland (GNOME) |
| Live-Vorschau des Bildschirms in der App | Funktioniert |
| App startet beim Einstecken des Buttons und schließt sich beim Abziehen | Funktioniert (systemd) |
| Ton im Raum | Funktioniert auf X11-Systemen; unter Wayland noch nicht getestet |

Getestete Systeme:

| Distribution | Sitzung | Wine | ClickShare-App | Button | Basis |
|---|---|---|---|---|---|
| Ubuntu 26.04 | GNOME 50, Wayland | 10.0 (Ubuntu-Paket) | 4.51.1 | Gen4 `0600:00ce` | CX-30 |
| Pop!_OS 22.04 | GNOME, X11 | 11.0 (WineHQ) | 4.36.0 | `0600:00ce` | nicht erfasst |

> **Hinweis:** Dies ist ein inoffizielles Community-Projekt. Es wird von Barco
> weder unterstützt noch empfohlen. „ClickShare“ und „Barco“ sind Marken der
> Barco N.V. Dieses Repository enthält **keine Barco-Software**; du brauchst
> einen eigenen Installer der ClickShare-App.

## Inhalt

- [Schnellstart](#schnellstart)
- [Funktionsweise](#funktionsweise)
- [Voraussetzungen](#voraussetzungen)
- [Installation](#installation)
- [Benutzung](#benutzung)
- [Einstellungen](#einstellungen)
- [Fehlerbehebung](#fehlerbehebung)
- [Einschränkungen](#einschränkungen)
- [Deinstallation](#deinstallation)
- [Entwicklung](#entwicklung)
- [Technische Details](docs/TECHNICAL.md)
- [Lizenz](#lizenz)

## Schnellstart

Mit installiertem Wine 9 oder neuer und `ClickShare_Setup.exe` in `~/Downloads`
([woher du die Datei bekommst](#2-clickshare_setupexe-besorgen)):

```sh
git clone https://github.com/praise2112/clickshare-linux.git
cd clickshare-linux
sudo sh scripts/install-udev-rule.sh
sh scripts/setup.sh ~/Downloads/ClickShare_Setup.exe
```

Stecke dann den Button ein. Die App öffnet sich nach etwa 15 Sekunden von
selbst.

## Funktionsweise

```
 Wayland: GNOME screen sharing (xdg-desktop-portal + PipeWire)
          -> helper/clickshare-capture.py -> frame file in /run/user/<uid>
 X11:     GDI BitBlt inside Wine
                         |
                         v
 +--------------------------+  DuplicateOutput()   +-----------------------------+
 | csdup.dll (shim)         |<---------------------| clickshare_native.exe       |
 | DXGI Desktop Duplication |--- D3D11 texture --->| (Barco's app, under Wine)   |
 +--------------------------+                      +--------------+--------------+
                                          HID over USB (hidraw)   | encoded video
                                                                  v
                     ClickShare Button --- Wi-Fi ---> base unit ---> room display
```

1. **Die App.** `clickshare_native.exe` ist ein natives Qt-Programm.
   `scripts/setup.sh` packt sie direkt aus Barcos `ClickShare_Setup.exe` aus.
   So umgeht es Barcos Installer, der .NET braucht und unter Wine hängt.
2. **Der Button.** Wine spricht die HID-Schnittstellen des Buttons über
   `hidraw` an. Eine udev-Regel gibt dem angemeldeten Benutzer Zugriff und
   startet außerdem die App, wenn ein Button eingesteckt wird.
3. **Bildschirmaufnahme.** Die App nimmt den Bildschirm ausschließlich über
   *DXGI Desktop Duplication* auf, und das hat Wine nicht implementiert.
   `csdup.dll` implementiert es. Unter Wayland sieht Wine den Bildschirm
   überhaupt nicht. Deshalb fragt ein kleines Python-Hilfsprogramm GNOME über
   das Portal für Bildschirmfreigabe nach dem Bildschirm (dasselbe Portal, das
   auch Browser nutzen) und reicht die Bilder an den Shim weiter. Unter X11
   liest der Shim den Bildschirm per GDI aus. `inject.exe` startet die App und
   lädt den Shim hinein.
4. **Ton.** Der Button ist zugleich eine USB-Soundkarte. Während einer Freigabe
   gibt er alles weiter, was an ihn ausgegeben wird. `clickshare.sh` macht ihn
   zur Standard-Audioausgabe.

Mehr Details: [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Voraussetzungen

- Ein **ClickShare Button**, der mit der Basis deines Besprechungsraums
  funktioniert.
- `ClickShare_Setup.exe`, der Windows-Installer der ClickShare-App
  ([Schritt 2](#2-clickshare_setupexe-besorgen)).
- **Wine 9 oder neuer.** Das Wine 6.0 aus Ubuntu 22.04 ist zu alt: Die App
  stürzt beim Start ab.
- Für Wayland: GNOME mit `xdg-desktop-portal-gnome`, PipeWire und das
  GStreamer-PipeWire-Plugin mit Python-Anbindung. Unter Ubuntu:
  `sudo apt install gstreamer1.0-pipewire python3-gi gir1.2-gstreamer-1.0`.
  Die meisten GNOME-Installationen haben das schon.
- `python3`, `curl` und `pactl` für die Audio-Weiterleitung (Paket
  `pulseaudio-utils`, das auch mit PipeWire funktioniert). Optional: `7z`
  (`p7zip-full`).
- Einmalig ein Passwort für `sudo`, für die udev-Regel (und für Wine, falls du
  es installieren musst).

## Installation

### 1. Wine

```sh
wine --version
```

Ist Wine älter als 9, installierst du das aktuelle WineHQ-Wine. Auf Ubuntu und
Ablegern wie Pop!_OS oder Linux Mint:

```sh
sudo sh scripts/install-winehq.sh
```

Für andere Distributionen siehe die
[Download-Seite von WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/Download).
Das eigene `wine`-Paket von Ubuntu 26.04 (10.0) funktioniert ohne Änderungen.

### 2. `ClickShare_Setup.exe` besorgen

Du brauchst den Windows-Installer der ClickShare-App. Entweder:

- Lade *ClickShare App for Windows* von Barcos Website herunter (barco.com →
  ClickShare → Apps / Downloads), oder
- stecke den Button ein und starte den Launcher, der darauf gespeichert ist:
  `wine /media/$USER/ClickShare/ClickShare_for_Windows.exe`. Nimm die Lizenz
  an und klicke auf *Install*. Der Launcher lädt `ClickShare_Setup.exe` nach
  `~/Downloads`. Die anschließende .NET-Installation dann **abbrechen**: Sie
  hängt unter Wine, und die App braucht sie nicht.

### 3. USB-Rechte und Autostart (udev)

```sh
sudo sh scripts/install-udev-rule.sh
```

Die Regel [`udev/70-clickshare.rules`](udev/70-clickshare.rules) gibt dem am
Rechner angemeldeten Benutzer Zugriff auf Barco-USB-Geräte (Hersteller-ID
`0600`). Bei ClickShare Buttons bittet sie außerdem systemd, die App zu
starten, sobald einer eingesteckt wird. **Den Button danach abziehen und wieder
einstecken.**

### 4. Einrichten

```sh
sh scripts/setup.sh ~/Downloads/ClickShare_Setup.exe
```

Das Skript:

1. Legt einen Wine-Prefix (eine eigene Windows-Umgebung) in
   `~/.wine-clickshare` an. Für einen anderen Ort startest du das Skript mit
   `CLICKSHARE_PREFIX=/some/path`; der Pfad wird in der
   [Einstellungsdatei](#einstellungen) gespeichert.
2. Packt die App mit
   [`scripts/extract-clickshare-app.py`](scripts/extract-clickshare-app.py) in
   den Prefix aus.
3. Baut `build/csdup.dll` und `build/inject.exe`. Ist MinGW-w64 nicht
   installiert, lädt es eine feste Zig-Version nach `.toolchain/`, ohne
   Root-Rechte.
4. Fügt **ClickShare** mit dem eigenen Symbol der App zu deiner
   Anwendungsübersicht hinzu.
5. Installiert einen systemd-Benutzerdienst, `clickshare.service`, der die App
   ausführt, solange ein Button eingesteckt ist.

Die Einrichtung hängt an dem Ort, an den du das Repository geklont hast.
Verschiebst du den Ordner, führe `sh scripts/setup.sh` an seinem neuen Ort
erneut aus.

## Benutzung

1. **Button einstecken.** Die App startet von selbst und zeigt *Share in the
   room* (Für den Raum freigeben), sobald sie sich mit dem Raum gekoppelt hat,
   nach etwa 15 Sekunden. Du kannst sie auch über die Anwendungsübersicht oder
   mit `sh clickshare.sh` starten. Öffnest du sie über die Anwendungsübersicht,
   während sie schon läuft, bleiben App und Freigabe unverändert.
2. **Auf Desktop 1 klicken** (oder den Button drücken). Unter Wayland öffnet
   sich GNOMEs Auswahldialog für die Freigabe:
   - Reiter **Window** (Fenster): ein einzelnes Fenster teilen.
   - Reiter **Display** (Bildschirm): deinen Bildschirm teilen, oder
     **Virtual Monitor** für einen separaten Bildschirm wählen.
3. **Stoppen oder pausieren** mit den Schaltflächen auf der Vorschau. **Das
   Abziehen des Buttons** schließt die App.

**Ein separater Bildschirm für den Raum.** Wähle *Virtual Monitor*: GNOME fügt
rechts neben deinem Bildschirm einen zusätzlichen Bildschirm hinzu, und nur der
Raumbildschirm zeigt ihn. Ziehe die Fenster, die du präsentieren willst, über
den rechten Rand deines Bildschirms hinaus. Dein eigener Bildschirm bleibt
privat.

**Die Bildschirmvorschau.** Beim ersten Start der App fragt GNOME, welchen
Bildschirm sie für die Vorschau von *Desktop 1* verwenden darf. Lass *Remember
this selection* (Auswahl merken) angehakt, dann fragt GNOME nicht noch einmal.
Die Vorschau aktualisiert sich, wenn die App sie neu zeichnet, zum Beispiel
wenn du zur App wechselst.

**Ton.** Solange die App läuft, ist der Button deine Standard-Audioausgabe, und
der Raum hört, was du während einer Freigabe abspielst. Stelle die Ausgabe
danach unter *Settings → Sound* (Einstellungen → Ton) wieder um.

## Einstellungen

Einstellungen stehen in `~/.config/clickshare-linux/env`, eine Zeile
`NAME=value` pro Einstellung. Der Launcher, der Autostart-Dienst und
`clickshare.sh` lesen alle diese Datei. Eine Variable aus der Umgebung hat
Vorrang vor der Datei.

| Einstellung | Wirkung |
|---|---|
| `CLICKSHARE_PREFIX=/path` | Zu verwendender Wine-Prefix (Standard `~/.wine-clickshare`) |
| `CLICKSHARE_NO_AUDIO=1` | Die Audioausgabe nicht verändern |
| `CLICKSHARE_MAX_FPS=20` | Nur Wayland: der App höchstens so viele Bilder pro Sekunde übergeben. Auf einem System verringerten 15-20 fps die Verzögerung beim Scrollen; der Standardwert `0` behält die eigene Bildrate der App bei. |
| `WINEDLLOVERRIDES=d3d11,dxgi=n` | Die DLLs von DXVK verwenden, siehe unten |

**Optional: DXVK.** [DXVK](https://github.com/doitsujin/dxvk) führt das
Direct3D der App über Vulkan aus. In unseren Tests senkte es die CPU-Last der
App von etwa 2 auf 1,2 Kerne und machte das Scrollen etwas flüssiger. Es
braucht einen funktionierenden Vulkan-Treiber. Lade dazu ein DXVK-Release
herunter und kopiere dessen 32-Bit-DLLs in den Prefix (die App ist 32-Bit).
Führe das im Ordner des Repositorys aus, damit der Prefix aus deinen
Einstellungen kommt:

```sh
PREFIX=$(sh -c '. scripts/env.sh; echo "${CLICKSHARE_PREFIX:-$HOME/.wine-clickshare}"')
tar xf dxvk-3.1.1.tar.gz
cp dxvk-3.1.1/x32/d3d11.dll dxvk-3.1.1/x32/dxgi.dll "$PREFIX/drive_c/windows/syswow64/"
mkdir -p ~/.config/clickshare-linux
echo 'WINEDLLOVERRIDES=d3d11,dxgi=n' >> ~/.config/clickshare-linux/env
```

## Fehlerbehebung

| Symptom | Ursache und Lösung |
|---|---|
| Beim Einstecken des Buttons passiert nichts | Prüfe `systemctl --user status clickshare`. Ist der Dienst unbekannt, führe `sh scripts/setup.sh` erneut aus. Startet er nie, führe `sudo sh scripts/install-udev-rule.sh` erneut aus und stecke den Button neu ein. |
| App meldet *„plug in a ClickShare Button“*, obwohl er eingesteckt ist | Kein HID-Zugriff. Wiederhole den udev-Schritt, stecke den Button neu ein und starte die App neu. Die App bemerkt keinen Button, der nach ihrem Start eingesteckt wurde; mit Autostart startet sie beim Neueinstecken neu. |
| GNOMEs Auswahldialog für die Freigabe erscheint nicht | Er kann sich hinter anderen Fenstern öffnen. Schau in der *Activities*-Übersicht nach. |
| Vorschau von *Desktop 1* bleibt schwarz | Unter Wayland hast du GNOMEs Frage zur Vorschau abgebrochen. Starte die App neu und erlaube es. |
| GNOME fragt erneut nach dem Bildschirm für die Vorschau | GNOME merkt sich die Antwort pro Programm, das die App gestartet hat. Launcher und Autostart zählen als eines; `sh clickshare.sh` aus einem Terminal zählt als ein anderes. |
| Raum zeigt den ClickShare-Rahmen, aber das **Bild ist schwarz** | Der Shim ist nicht geladen. In `$WINEPREFIX/drive_c/csdup.log` müssen `patched DuplicateOutput` und `duplication created` stehen. Starte die App nur über den Launcher oder `clickshare.sh`. |
| **Kein Ton** im Raum | `pactl` fehlt, oder die Standardausgabe ist nicht der Button: `pactl get-default-sink` sollte `…Barco_ClickShare…` nennen. Ton kommt nur während einer Freigabe im Raum an. |
| App stürzt beim Start ab (`RoGetActivationFactory`, `c0000005`) | Wine ist zu alt. Installiere Wine 9 oder neuer. |
| Dialog `.NET Framework Initialization Error` | Harmlos. Mit OK schließen. |
| .NET-Installer hängt bei *„netfx_Full_LDR.mzz“* | Du hast `ClickShare_Setup.exe` direkt ausgeführt. Nutze stattdessen `scripts/setup.sh` und beende Wine mit `wineserver -k`. |

**Logs:**

- App: `$WINEPREFIX/drive_c/users/$USER/AppData/Local/Barco/ClickShare App/clickshare.log`
- Shim: `$WINEPREFIX/drive_c/csdup.log`
- Autostart und Hilfsprogramm für die Bildschirmaufnahme: `journalctl --user -u clickshare`
- Wine: `WINEDEBUG=err,+hid sh clickshare.sh`

## Einschränkungen

- **Wayland wurde nur unter GNOME getestet.** Andere Desktops mit
  ScreenCast-Portal (zum Beispiel KDE Plasma) funktionieren vielleicht, aber
  der separate virtuelle Bildschirm und die Vorschau hängen vom Verhalten von
  GNOME ab.
- **Die eigene Fensterliste der App ist unter Wayland ausgeblendet.** Wine
  sieht dort die Fenster anderer Programme nicht. Wähle Fenster deshalb in
  GNOMEs Auswahldialog. Unter X11 ist das Teilen einzelner Fenster ungetestet.
- **Schnelle Bewegung senkt die Bildrate.** Folien und Dokumente laufen mit
  etwa 30 fps. Beim schnellen Scrollen läuft unter Wine die USB-Verbindung
  zum Button voll, und die App sendet 16-19 fps. Die Verzögerung vom Laptop zum
  Raumbildschirm lag in unseren Tests bei etwa 0,35-0,45 s; Barco veröffentlicht
  keinen Vergleichswert.
- **Der Start dauert etwa 15 Sekunden**, vor allem durch den Start der App
  selbst und die Kopplung mit dem Raum.
- Funktionen, die .NET brauchen (Kalender- und Outlook-Integration), und
  automatische Updates sind nicht verfügbar. Zum Aktualisieren führst du
  `setup.sh` mit einem neueren Installer aus.
- Teilen über das Netzwerk ohne Button ist ungetestet.
- Setups mit mehreren Monitoren sind ungetestet.

## Deinstallation

Führe das im Ordner des Repositorys aus. `scripts/uninstall.sh` beendet die
App, entfernt dann den Autostart-Dienst, den Launcher, das Symbol und die
Einstellungen und fragt nach, bevor es den Wine-Prefix mit der ClickShare-App
löscht.

```sh
sh scripts/uninstall.sh
sudo rm /etc/udev/rules.d/70-clickshare.rules && sudo udevadm control --reload
rm -rf build .toolchain                    # build output in this repository
```

## Entwicklung

```sh
sh scripts/build.sh     # builds the shim into build/
sh tests/run.sh         # runs the tests
```

Aufbau des Repositorys:

```
clickshare.sh                     starts the app: settings, audio, capture helper, shim
helper/clickshare-capture.py      Wayland screen capture through the ScreenCast portal
shim/csdup.c                      DXGI Desktop Duplication for Wine, thumbnail and window-list fixes
shim/inject.c                     starts a program and loads a DLL into it
shim/pacing.h                     frame pacing for CLICKSHARE_MAX_FPS
shim/dupltest.c                   checks screen capture without the app (build/dupltest.exe)
scripts/setup.sh                  one-time setup: prefix, app, build, launcher, autostart
scripts/build.sh                  builds the shim (MinGW-w64 or a downloaded Zig)
scripts/extract-clickshare-app.py unpacks the app from ClickShare_Setup.exe
scripts/extract-icon.py           saves the app's icon for the launcher
scripts/install-launcher.sh       adds ClickShare to the app grid
scripts/launch.sh                 what the launcher runs; never stops a running app
scripts/uninstall.sh              removes what setup.sh installed
scripts/install-autostart.sh      installs clickshare.service
scripts/install-udev-rule.sh      installs udev/70-clickshare.rules
scripts/install-winehq.sh         installs current Wine from WineHQ (Ubuntu family)
scripts/env.sh                    reads ~/.config/clickshare-linux/env, finds wineserver
udev/70-clickshare.rules          device access and autostart
tests/                            tests, run with tests/run.sh
docs/TECHNICAL.md                 analysis and design notes
```

Berichte von anderen Distributionen, Desktops, Button-Modellen
(`lsusb | grep 0600`) und App-Versionen sind willkommen. Bitte gib deine
Wine-Version, das App-Log und `csdup.log` mit an.

## Danksagung

Basiert auf [ProjektW2/clickshare-linux](https://github.com/ProjektW2/clickshare-linux)
von Christoph Bellmann. Dieses Projekt hat die App unter Wine auf X11 zum
Laufen gebracht und den DXGI-Shim geschrieben. Dieser Fork ergänzt die
Wayland-Aufnahme, die Korrekturen für Vorschau und Fensterliste, den
virtuellen Bildschirm, die Taktung der Bilder (frame pacing), den Autostart
und den Launcher.

## Lizenz

Der Code in diesem Repository steht unter der [MIT-Lizenz](LICENSE). Die
ClickShare-Software ist proprietäre Software der Barco N.V. und fällt **nicht**
unter diese Lizenz. Für ihre Nutzung gelten Barcos Lizenzbedingungen.
