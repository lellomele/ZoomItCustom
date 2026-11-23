[Italiano](README.it.md) | [English](README.md)

# ZoomIt Custom

Strumento portatile per ingrandire e annotare lo schermo durante lezioni e presentazioni. La versione Custom privilegia la stabilità del cursore, la rapidità delle operazioni frequenti e un consumo di memoria contenuto.

## Primo avvio

Estrai lo [ZIP portatile](https://github.com/lellomele/ZoomItCustom/releases/latest) e tieni insieme i due eseguibili. Avvia **ZoomItCustom.exe** per l’uso normale. Se desideri il riavvio automatico dopo un crash, avvia **ZoomItCustomSupervisor.exe**. Il Supervisor apre l’app e prova a recuperare l’ultimo disegno completato, in pausa. Il tratto in corso e la cronologia di Annulla vanno persi. Dal menu vicino all’orologio puoi chiudere entrambi o configurare l’avvio automatico all’accesso.

## Scorciatoie predefinite

| Tasti | Azione |
| --- | --- |
| Ctrl+1 | Congela e ingrandisce lo schermo |
| Ctrl+2 | LiveZoom. Ctrl+rotellina regola lo zoom ed Esc esce, anche a 1× |
| Ctrl+3 / Ctrl+Maiusc+3 | Disegna su un’immagine fissa / sul desktop in movimento |
| Ctrl+4 | Timer per la pausa |
| Ctrl+5 / Ctrl+Maiusc+5 | Copia una selezione negli appunti / salvala in PNG |
| Ctrl+6 | Apre o chiude la lavagna. C cambia lo sfondo e Ctrl+rotellina regola la griglia |

Le scorciatoie sono configurabili dalle opzioni. In Draw, il clic destro sospende il disegno. Il clic sinistro riprende, un secondo clic destro termina. Sulla lavagna, Esc termina Draw e cancella le annotazioni.

## Recupero e requisiti

Dopo un reset video, LiveZoom prova a ripristinare ingrandimento e posizione. Un cambiamento della geometria dello schermo può terminare il disegno. Solo crash ed errori gravi generano rapporti in `%LOCALAPPDATA%\ZoomItCustom\Logs` (percorso alternativo `%TEMP%\ZoomItCustom-Logs`).

Serve Windows 10 o 11 aggiornato a 64 bit. Gli eseguibili sono privi di firma digitale. Per mostrare il Timer separatamente sul secondo schermo serve il desktop esteso. Il Supervisor può recuperare immagini del disegno fino a 64 MiB ciascuna e interviene quando il processo termina.

## Compilazione e licenza

Esegui `.\build.ps1` con Visual Studio 2022 o successivo, strumenti C++, Windows SDK e CMake. Trovi i due eseguibili in `build-Release`.

Basato su [Microsoft PowerToys ZoomIt](https://github.com/microsoft/PowerToys/tree/21fd5092b3e062ca6c8dd6b8c772a236f90b3b42/src/modules/ZoomIt/ZoomIt). Codice originale © Mark Russinovich / Microsoft Corporation. Modifiche Custom © 2026 Prof. ing. Raffaele Mele. [Licenza MIT](LICENSE).
