// gate_types.h -- Typen fuer Gate g / Override-Watchdog.
// In eigenes Header ausgelagert, damit der Arduino-Auto-Prototype-Generator den Typ kennt:
// Auto-Prototypen werden HINTER die #include-Zeilen gehoben -> der per #include gebrachte Typ ist
// vor jedem generierten Prototyp definiert (loest "SentRec was not declared" fuer devPublish/wdCheck).
#pragma once

struct SentRec {   // Watchdog: zuletzt vom Broker SELBST gesendeter {Wert, Zeit} je Geraete-/set-Topic
  char          pl[16];
  unsigned long t;
};
