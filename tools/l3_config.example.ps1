# l3_config.example.ps1 -- Vorlage. Kopieren nach l3_config.ps1 und ausfuellen.
# l3_config.ps1 ist .gitignore't (enthaelt IPs + Notification-Zugangsdaten).
$L3 = @{
  # --- Steuer-Domaene (read-only /status pollen) ---
  ReglerHost  = "192.168.188.xx"   # Regler-IP
  BrokerHost  = "192.168.188.xx"   # Broker-IP

  # --- Verhalten ---
  PollSeconds = 20                 # Poll-Intervall
  SocAlert    = 35                 # % - Alarm wenn SoC <= das (naehert sich Floor 30)
  RenotifyMin = 30                 # gleiche Bedingung fruehestens nach X min erneut melden (Anti-Spam)
  FailStrikes = 2                  # so viele Poll-Fehler in Folge, bevor "unerreichbar" alarmiert (Netz-Blip filtern)

  # --- Notification-Kanal: "telegram" ODER "email" ---
  Channel = "telegram"

  # Telegram (Empfehlung: Push aufs Handy). Setup: @BotFather -> /newbot -> Token; Chat-ID via @userinfobot.
  TgToken = "123456:ABC-DEF..."    # Bot-Token
  TgChat  = "123456789"            # deine Chat-ID

  # E-Mail (Alternative, z.B. GMX). Braucht App-Passwort (nicht das Login-Passwort!).
  SmtpServer = "mail.gmx.net"; SmtpPort = 587
  MailFrom = "you@gmx.de"; MailTo = "you@gmx.de"
  MailUser = "you@gmx.de"; MailPass = "APP_PASSWORT"
}
