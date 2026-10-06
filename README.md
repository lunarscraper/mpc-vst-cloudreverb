# mpc-vst-cloudreverb

**Cloud Reverb** – ein leichter Hall für lange, bewegte Klangteppiche (Ambient, Chill-out, Soft Drone) als VST2-**Insert-Effekt** für den Plugin-Host von Akai MPC OS (Force, MPC Live/One/X/Key). Sieben Typen, neun Regler (plus Grundton und Akkord für den Typ MODAL), 32 Speicherplätze.

**Status:** Offline-Test (x86, ASan/UBSan) besteht. Actions-Build, Skin und Test auf der Force stehen aus. Gehört hat das Plugin noch niemand: Regelwege, Pegel und Werks-Presets sind nach Messwerten eingestellt, nicht nach Gehör.

## Herkunft

Die Richtung sind die flächigen Maschinen eines Strymon BigSky (Cloud, Bloom, Swell, Shimmer). Nachgebaut ist davon nichts, es gibt weder Messungen noch einen Hörvergleich mit dem Gerät. Der Aufbau der Hallfahne folgt [CloudSeed](https://github.com/ValdemarOrn/CloudSeed) von Valdemar Erlingsson (Diffusoren in parallele, modulierte Rückkopplungslinien). Die Typen CHOIR und MODAL sind von zwei Patches des [Phonicbloom Siluria](https://phonicbloom.com/siluria/) angeregt (Formant und Modal, Firmware 1.2), nach deren Beschreibung im Handbuch. Der Code ist durchweg neu geschrieben; aus CloudSeed, CloudReverb oder Siluria ist nichts übernommen.

## Die sieben Typen

TYPE steht auf dem Tab PRESET. **CHARACTER** ist der eine Regler, dessen Aufgabe vom Typ abhängt; die Anzeige sagt, was er gerade ist.

| TYPE | Wirkung | CHARACTER |
|---|---|---|
| **CLOUD** | Dichter, weicher Hall. Mit Bloom setzt er nicht ein, sondern wächst langsam heran. | **BLOOM** 0–100 % |
| **SWELL** | Jeder neue Ton wird in den Hall eingeblendet, die Anschläge verschwinden. Ausgelöst von einem Pegelsprung oder von Klang nach Stille. | **SWELL** 0,1–4 s Einblendzeit |
| **SHIMMER** | Die Fahne wird eine Oktave höher wieder eingespeist und steigt mit jeder Runde weiter auf. | **SHIMMER** 0–100 % |
| **TIDE** | Langsame Wellen aus Lautstärke und Helligkeit laufen über die Fahne, links und rechts um eine Viertelwelle versetzt. | **WAVE** 30–2 s pro Welle |
| **DRONE** | Die Fahne hält sich selbst. Bei 100 % bleibt sie stehen, und je voller sie ist, desto weniger Neues kommt hinein. | **HOLD** 0–100 % |
| **CHOIR** | Die Fahne läuft durch drei Vokal-Formanten je Seite und klingt wie gesummt. Kein Vocoder: der Klang kommt weiter aus dem Eingangssignal. | **VOWEL** A – E – I – O – U |
| **MODAL** | Vor dem Hall regt das Eingangssignal eine Bank gestimmter Resonanzen an (vier Töne eines Akkords, je sechs Teiltöne) und klingt in diesem Akkord zurück. Die Bank hat keine Rückkopplung und kann nicht aufschwingen. | Lage der Teiltöne: **GAMELAN** (gestaucht) – **STRING** (harmonisch, bei 50 %) – **BELL** (gespreizt) |

Ein TYPE-Wechsel lässt alle Regler stehen.

Nur für MODAL gibt es auf dem Tab PRESET zwei weitere Bedienelemente: **ROOT** (Grundton C bis B, Oktave ab C3) und **CHORD** (OCTAVES, FIFTHS, MAJOR, MINOR, SUS 2, SUS 4, MAJOR 7, MINOR 7, TANPURA = Pa–Sa–Sa–tiefes Sa). DECAY bestimmt bei MODAL auch, wie lange die Resonanzen klingen (halbe Nachhallzeit, 0,15 bis 8 s). Ein Ton, der genau auf einer Resonanz liegt, schwillt stark an und wird dann weich begrenzt.

Für wellenartige Flächen: TIDE direkt, oder SWELL und CLOUD mit viel Bloom, wenn die Wellen aus dem Spiel kommen sollen. DRONE mit HOLD 100 % friert ein, was gerade klingt; mit MODULATION über 0 verliert der gehaltene Klang langsam Höhen (gemessen bei MODULATION 40: rund 6 dB in 25 s), bei 0 bleibt er.

## Regler

Tab REVERB, in der Reihenfolge der acht Force-Knöpfe:

| Regler | Bereich | Wirkung |
|---|---|---|
| **MIX** | 0–100 % | Bis 50 % bleibt das Original voll und der Hall kommt dazu, darüber wird das Original leiser. 100 % = nur Hall. Bei 0 % geht das Signal bitgenau durch. |
| **SIZE** | 0–100 % | Raumgröße: Länge der sechs Linien (Mitte 25 bis 600 ms) und der Allpässe. |
| **DECAY** | 0,2–60 s | Nachhallzeit. |
| **CHARACTER** | je nach TYPE | siehe oben |
| **DIFFUSION** | 0–100 % | 0 = einzelne Echos, 100 = dichte, weiche Fahne. |
| **MODULATION** | 0–100 % | Bewegung in den Linien, Tiefe und Tempo zusammen. Hoch = schwebend, leicht verstimmt. |
| **TONE** | 400 Hz – OPEN | Höhendämpfung in der Fahne. |
| **LOW CUT** | 20 Hz – 1 kHz | Hochpass vor dem Hall. |

Tab PRESET: **TYPE**, **PRE-DELAY** (0–250 ms), **ROOT** und **CHORD** (nur MODAL) und die Speicherplätze.

SIZE und PRE-DELAY gleiten beim Drehen wie ein Bandecho (kurze Tonhöhenfahrt), statt zu knacken. Das trockene Signal wird nie verändert. Der Hallanteil allein wird oberhalb von −3 dBFS weich begrenzt und bleibt unter 0,99.

## Speicherplätze

32 Plätze, die auch in der PRESET-Liste der Force erscheinen. Der Regler PRESET blättert nur, **LOAD** lädt, **SAVE** überschreibt den gewählten Platz. Die Plätze gelten für alle Instanzen und Projekte und liegen in `cloudreverb_presets.txt` neben dem Plugin-Ordner auf der SD-Karte. Das Projekt speichert die Reglerstellungen zusätzlich selbst.

Plätze 1–21 sind ab Werk belegt, bis man sie überschreibt:

| Nr. | Name | TYPE |
|---|---|---|
| 1–3 | SOFT CLOUD, SLOW BLOOM, DEEP BLOOM | CLOUD |
| 4–5 | GENTLE SWELL, LONG SWELL | SWELL |
| 6–7 | SHIMMER HAZE, SHIMMER CHOIR | SHIMMER |
| 8–10 | SLOW TIDE, BREATHING, RIPPLES | TIDE |
| 11–13 | SOFT DRONE, ETERNAL, DARK DRONE | DRONE |
| 14 | WARM HALL | CLOUD ohne Bloom |
| 15–16 | SEND CLOUD (WET), SEND TIDE (WET) | nur Hall, für Send-Betrieb |
| 17–18 | CHOIR AH, CHOIR OOH | CHOIR |
| 19–21 | SYMPATHETIC (D, Tanpura), SINGING BOWLS (D, Quinten, Glocke), GAMELAN MIST (A, Sus 2) | MODAL |

## Messwerte

Vom x86-Entwicklungsrechner, auf der Force noch nicht gemessen.

- CPU, ein Kern, Stereo, alles auf Anschlag, im Verhältnis zu CLOUD: TIDE 1,06-fach, SWELL 1,13, CHOIR 1,16, SHIMMER 1,21, DRONE 1,23, MODAL 1,29. CLOUD selbst lag je nach Auslastung des Messrechners zwischen 0,6 % und 1,5 % eines Kerns.
- Nachhallzeit bei Anzeige 1,11 / 3,46 / 10,8 s: gemessen 1,24 / 3,59 / 10,5 s.
- Bloom 100 %: die Hälfte der Fahnenenergie ist nach 1,24 s da statt nach 0,43 s.
- Shimmer 100 %: nach einem 220-Hz-Ton liegen 440 und 880 Hz in der Fahne rund 7 dB über dem Ton selbst.
- Tide: Lautstärke schwankt um rund 24 dB, links und rechts unabhängig.
- Choir: bei Vokal A liegen 730 und 1090 Hz rund 20 dB über 270 Hz, bei Vokal I liegt 270 Hz über 40 dB über 730 Hz.
- Modal, Grundton A, Quinten: nach einem Rauschstoß liegt 220 Hz in der Fahne 38 dB über 277 Hz (Cis, nicht im Akkord). Ein 220-Hz-Ton auf der Resonanz bleibt bei längster Klingzeit unter 0,99.
- Drone, HOLD 100 %, MODULATION 0: nach 25 s Stille 0,5 dB leiser. 40 s lautes Rauschen in vollen Hold bleiben begrenzt (Spitze 0,54).

## Bauen und testen

Build und Installation wie bei den anderen Plugins über GitHub Actions (Workflow „VST release (draft)"), siehe `docs/PORTING.md` in `sd88me/mpc-vst-plugins`.

- `vst/build.sh` baut `cloudreverb.so` (armhf) und den Skin.
- `vst/test.sh` fährt den Offline-Test (`vst/host_test.cpp`, die Liste steht im Dateikopf) und danach den Benchmark je TYPE.

## Lizenz

MIT, siehe `LICENSE`. Keine Verbindung zu Strymon, Ghost Note Audio (CloudSeed) oder zum CloudReverb-Projekt.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
