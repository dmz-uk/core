/* Copyright (c) 2009-2017 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "module-dir.h"
#include "iostream-ssl-private.h"


static bool ssl_module_loaded = FALSE;
#ifdef HAVE_SSL
static struct module *ssl_module = NULL;
#endif
static const struct iostream_ssl_vfuncs *ssl_vfuncs = NULL;

#ifdef HAVE_SSL
static void ssl_module_unload(void)
{
	module_dir_unload(&ssl_module);
}
#endif

void iostream_ssl_module_init(const struct iostream_ssl_vfuncs *vfuncs)
{
	ssl_vfuncs = vfuncs;
	ssl_module_loaded = TRUE;
}

int ssl_module_load(const char **error_r)
{
#ifdef HAVE_SSL
	const char *plugin_name = "ssl_iostream_openssl";
	struct module_dir_load_settings mod_set;

	i_zero(&mod_set);
	mod_set.abi_version = DOVECOT_ABI_VERSION;
	mod_set.setting_name = "<built-in lib-ssl-iostream lookup>";
	mod_set.require_init_funcs = TRUE;
	ssl_module = module_dir_load(MODULE_DIR, plugin_name, &mod_set);
	if (module_dir_try_load_missing(&ssl_module, MODULE_DIR, plugin_name,
					&mod_set, error_r) < 0)
		return -1;
	module_dir_init(ssl_module);
	if (!ssl_module_loaded) {
		*error_r = t_strdup_printf(
			"%s didn't call iostream_ssl_module_init() - SSL not initialized",
			plugin_name);
		module_dir_unload(&ssl_module);
		return -1;
	}

	/* Destroy SSL module after (most of) the others. Especially lib-fs
	   backends may still want to access SSL module in their own
	   atexit-callbacks. */
	lib_atexit_priority(ssl_module_unload, LIB_ATEXIT_PRIORITY_LOW);
	return 0;
#else
	*error_r = "SSL support not compiled in";
	return -1;
#endif
}

int ssl_iostream_context_init_client(const struct ssl_iostream_settings *set,
				     struct ssl_iostream_context **ctx_r,
				     const char **error_r)
{
	if (!ssl_module_loaded) {
		if (ssl_module_load(error_r) < 0)
			return -1;
	}
	return ssl_vfuncs->context_init_client(set, ctx_r, error_r);
}

int ssl_iostream_context_init_server(const struct ssl_iostream_settings *set,
				     struct ssl_iostream_context **ctx_r,
				     const char **error_r)
{
	if (!ssl_module_loaded) {
		if (ssl_module_load(error_r) < 0)
			return -1;
	}
	return ssl_vfuncs->context_init_server(set, ctx_r, error_r);
}

void ssl_iostream_context_deinit(struct ssl_iostream_context **_ctx)
{
	struct ssl_iostream_context *ctx = *_ctx;

	*_ctx = NULL;
	ssl_vfuncs->context_deinit(ctx);
}

int io_stream_create_ssl_client(struct ssl_iostream_context *ctx, const char *host,
				const struct ssl_iostream_settings *set,
				struct istream **input, struct ostream **output,
				struct ssl_iostream **iostream_r,
				const char **error_r)
{
	struct ssl_iostream_settings set_copy = *set;
	set_copy.verify_remote_cert = TRUE;
	return ssl_vfuncs->create(ctx, host, &set_copy, input, output,
				  iostream_r, error_r);
}

int io_stream_create_ssl_server(struct ssl_iostream_context *ctx,
				const struct ssl_iostream_settings *set,
				struct istream **input, struct ostream **output,
				struct ssl_iostream **iostream_r,
				const char **error_r)
{
	return ssl_vfuncs->create(ctx, NULL, set, input, output,
				  iostream_r, error_r);
}

void ssl_iostream_unref(struct ssl_iostream **_ssl_io)
{
	struct ssl_iostream *ssl_io = *_ssl_io;

	*_ssl_io = NULL;
	ssl_vfuncs->unref(ssl_io);
}

void ssl_iostream_destroy(struct ssl_iostream **_ssl_io)
{
	struct ssl_iostream *ssl_io = *_ssl_io;

	*_ssl_io = NULL;
	ssl_vfuncs->destroy(ssl_io);
}

void ssl_iostream_set_log_prefix(struct ssl_iostream *ssl_io,
				 const char *prefix)
{
	ssl_vfuncs->set_log_prefix(ssl_io, prefix);
}

int ssl_iostream_handshake(struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->handshake(ssl_io);
}

void ssl_iostream_set_handshake_callback(struct ssl_iostream *ssl_io,
					 ssl_iostream_handshake_callback_t *callback,
					 void *context)
{
	ssl_vfuncs->set_handshake_callback(ssl_io, callback, context);
}

bool ssl_iostream_is_handshaked(const struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->is_handshaked(ssl_io);
}

bool ssl_iostream_has_handshake_failed(const struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->has_handshake_failed(ssl_io);
}

bool ssl_iostream_has_valid_client_cert(const struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->has_valid_client_cert(ssl_io);
}

bool ssl_iostream_has_broken_client_cert(struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->has_broken_client_cert(ssl_io);
}

int ssl_iostream_cert_match_name(struct ssl_iostream *ssl_io, const char *name)
{
	return ssl_vfuncs->cert_match_name(ssl_io, name);
}

int ssl_iostream_check_cert_validity(struct ssl_iostream *ssl_io,
				     const char *host, const char **error_r)
{
	if (!ssl_iostream_has_valid_client_cert(ssl_io)) {
		if (!ssl_iostream_has_broken_client_cert(ssl_io))
			*error_r = "SSL certificate not received";
		else {
			*error_r = t_strdup(ssl_iostream_get_last_error(ssl_io));
			if (*error_r == NULL)
				*error_r = "Received invalid SSL certificate";
		}
		return -1;
	} else if (ssl_iostream_cert_match_name(ssl_io, host) < 0) {
		*error_r = t_strdup_printf(
			"SSL certificate doesn't match expected host name %s",
			host);
		return -1;
	}
	return 0;
}

const char *ssl_iostream_get_peer_name(struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->get_peer_name(ssl_io);
}

const char *ssl_iostream_get_server_name(struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->get_server_name(ssl_io);
}

const char *ssl_iostream_get_compression(struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->get_compression(ssl_io);
}

const char *ssl_iostream_get_security_string(struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->get_security_string(ssl_io);
}

const char *ssl_iostream_get_last_error(struct ssl_iostream *ssl_io)
{
	return ssl_vfuncs->get_last_error(ssl_io);
}

struct ssl_iostream_settings *
ssl_iostream_settings_dup(pool_t pool,
			  const struct ssl_iostream_settings *old_set)
{
	struct ssl_iostream_settings *new_set;

	new_set = p_new(pool, struct ssl_iostream_settings, 1);
	memcpy(new_set, old_set, sizeof(*new_set));

	new_set->protocols = p_strdup(pool, old_set->protocols);
	new_set->cipher_list = p_strdup(pool, old_set->cipher_list);
	new_set->curve_list = p_strdup(pool, old_set->curve_list);
	new_set->ca = p_strdup(pool, old_set->ca);
	new_set->ca_file = p_strdup(pool, old_set->ca_file);
	new_set->ca_dir = p_strdup(pool, old_set->ca_dir);
	new_set->cert.cert = p_strdup(pool, old_set->cert.cert);
	new_set->cert.key = p_strdup(pool, old_set->cert.key);
	new_set->cert.key_password = p_strdup(pool, old_set->cert.key_password);
	new_set->alt_cert.cert = p_strdup(pool, old_set->alt_cert.cert);
	new_set->alt_cert.key = p_strdup(pool, old_set->alt_cert.key);
	new_set->alt_cert.key_password = p_strdup(pool, old_set->alt_cert.key_password);
	new_set->cert_username_field = p_strdup(pool, old_set->cert_username_field);
	new_set->crypto_device = p_strdup(pool, old_set->crypto_device);

	return new_set;
}
