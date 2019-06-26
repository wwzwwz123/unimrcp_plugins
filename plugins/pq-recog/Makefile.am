AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = pqrecog.la

pqrecog_la_SOURCES       = src/pq_recog_engine.c
pqrecog_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS) \
			    -lcurl

include $(top_srcdir)/build/rules/uniplugin.am
