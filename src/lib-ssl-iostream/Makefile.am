noinst_LTLIBRARIES = libssl_iostream.la

NOPLUGIN_LDFLAGS =

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-DMODULE_DIR=\""$(moduledir)"\"

if BUILD_OPENSSL
module_LTLIBRARIES = libssl_iostream_openssl.la

libssl_iostream_openssl_la_LDFLAGS = -module -avoid-version
libssl_iostream_openssl_la_LIBADD = $(SSL_LIBS)
libssl_iostream_openssl_la_SOURCES = \
	dovecot-openssl-common.c \
	iostream-openssl.c \
	iostream-openssl-common.c \
	iostream-openssl-context.c \
	istream-openssl.c \
	ostream-openssl.c
endif

libssl_iostream_la_SOURCES = \
	iostream-ssl.c

noinst_HEADERS = \
	dovecot-openssl-common.h

headers = \
	iostream-openssl.h \
	iostream-ssl.h \
	iostream-ssl-private.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)
