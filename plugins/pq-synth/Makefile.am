AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = pqsynth.la

pqsynth_la_SOURCES       = src/pq_synth_engine.c
pqsynth_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS) \
                            -lcurl -lxml2 

include $(top_srcdir)/build/rules/uniplugin.am
