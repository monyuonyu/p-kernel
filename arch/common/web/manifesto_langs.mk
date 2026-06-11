# manifesto_langs.mk — the single source of truth for the i18n manifesto
# table (i18n wave). Included by every boot/*/Makefile so the language set
# never drifts per target. Format: <code>:<endonym>:<file>. ja is FIRST
# (canonical/default) — its bytes stay byte-identical to manifesto.txt.
#
# MANIFESTO_LANGS_FULL  — hosted (web UI): all ~32 languages.
# MANIFESTO_LANGS_LEAN  — bare-metal: ja+en only (no web UI; lean kernel;
#                         the manifesto there is for the future
#                         netstack-tcp-server slice). See ark-profile.md §7.5.
MANIFESTO_WEB := $(ARCH_COMMON)/web

MANIFESTO_LANGS_FULL := \
  ja:日本語:$(MANIFESTO_WEB)/manifesto.txt \
  en:English:$(MANIFESTO_WEB)/manifesto.en.txt \
  zh-Hans:简体中文:$(MANIFESTO_WEB)/manifesto.zh-Hans.txt \
  zh-Hant:繁體中文:$(MANIFESTO_WEB)/manifesto.zh-Hant.txt \
  hi:हिन्दी:$(MANIFESTO_WEB)/manifesto.hi.txt \
  es:Español:$(MANIFESTO_WEB)/manifesto.es.txt \
  fr:Français:$(MANIFESTO_WEB)/manifesto.fr.txt \
  ar:العربية:$(MANIFESTO_WEB)/manifesto.ar.txt \
  bn:বাংলা:$(MANIFESTO_WEB)/manifesto.bn.txt \
  pt:Português:$(MANIFESTO_WEB)/manifesto.pt.txt \
  ru:Русский:$(MANIFESTO_WEB)/manifesto.ru.txt \
  ur:اردو:$(MANIFESTO_WEB)/manifesto.ur.txt \
  id:Bahasa\ Indonesia:$(MANIFESTO_WEB)/manifesto.id.txt \
  de:Deutsch:$(MANIFESTO_WEB)/manifesto.de.txt \
  sw:Kiswahili:$(MANIFESTO_WEB)/manifesto.sw.txt \
  mr:मराठी:$(MANIFESTO_WEB)/manifesto.mr.txt \
  te:తెలుగు:$(MANIFESTO_WEB)/manifesto.te.txt \
  tr:Türkçe:$(MANIFESTO_WEB)/manifesto.tr.txt \
  ta:தமிழ்:$(MANIFESTO_WEB)/manifesto.ta.txt \
  vi:Tiếng\ Việt:$(MANIFESTO_WEB)/manifesto.vi.txt \
  ko:한국어:$(MANIFESTO_WEB)/manifesto.ko.txt \
  it:Italiano:$(MANIFESTO_WEB)/manifesto.it.txt \
  th:ไทย:$(MANIFESTO_WEB)/manifesto.th.txt \
  fil:Filipino:$(MANIFESTO_WEB)/manifesto.fil.txt \
  pl:Polski:$(MANIFESTO_WEB)/manifesto.pl.txt \
  fa:فارسی:$(MANIFESTO_WEB)/manifesto.fa.txt \
  uk:Українська:$(MANIFESTO_WEB)/manifesto.uk.txt \
  ms:Bahasa\ Melayu:$(MANIFESTO_WEB)/manifesto.ms.txt \
  pa:ਪੰਜਾਬੀ:$(MANIFESTO_WEB)/manifesto.pa.txt \
  ro:Română:$(MANIFESTO_WEB)/manifesto.ro.txt \
  nl:Nederlands:$(MANIFESTO_WEB)/manifesto.nl.txt \
  gu:ગુજરાતી:$(MANIFESTO_WEB)/manifesto.gu.txt

MANIFESTO_LANGS_LEAN := \
  ja:日本語:$(MANIFESTO_WEB)/manifesto.txt \
  en:English:$(MANIFESTO_WEB)/manifesto.en.txt

# all the source files (for the .h dependency)
MANIFESTO_SRCS := \
  $(MANIFESTO_WEB)/manifesto.txt \
  $(MANIFESTO_WEB)/manifesto.en.txt \
  $(MANIFESTO_WEB)/manifesto.zh-Hans.txt $(MANIFESTO_WEB)/manifesto.zh-Hant.txt \
  $(MANIFESTO_WEB)/manifesto.hi.txt $(MANIFESTO_WEB)/manifesto.es.txt \
  $(MANIFESTO_WEB)/manifesto.fr.txt $(MANIFESTO_WEB)/manifesto.ar.txt \
  $(MANIFESTO_WEB)/manifesto.bn.txt $(MANIFESTO_WEB)/manifesto.pt.txt \
  $(MANIFESTO_WEB)/manifesto.ru.txt $(MANIFESTO_WEB)/manifesto.ur.txt \
  $(MANIFESTO_WEB)/manifesto.id.txt $(MANIFESTO_WEB)/manifesto.de.txt \
  $(MANIFESTO_WEB)/manifesto.sw.txt $(MANIFESTO_WEB)/manifesto.mr.txt \
  $(MANIFESTO_WEB)/manifesto.te.txt $(MANIFESTO_WEB)/manifesto.tr.txt \
  $(MANIFESTO_WEB)/manifesto.ta.txt $(MANIFESTO_WEB)/manifesto.vi.txt \
  $(MANIFESTO_WEB)/manifesto.ko.txt $(MANIFESTO_WEB)/manifesto.it.txt \
  $(MANIFESTO_WEB)/manifesto.th.txt $(MANIFESTO_WEB)/manifesto.fil.txt \
  $(MANIFESTO_WEB)/manifesto.pl.txt $(MANIFESTO_WEB)/manifesto.fa.txt \
  $(MANIFESTO_WEB)/manifesto.uk.txt $(MANIFESTO_WEB)/manifesto.ms.txt \
  $(MANIFESTO_WEB)/manifesto.pa.txt $(MANIFESTO_WEB)/manifesto.ro.txt \
  $(MANIFESTO_WEB)/manifesto.nl.txt $(MANIFESTO_WEB)/manifesto.gu.txt
