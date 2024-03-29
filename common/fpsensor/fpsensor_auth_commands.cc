/* Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "crypto/cleanse_wrapper.h"
#include "crypto/elliptic_curve_key.h"
#include "ec_commands.h"
#include "fpsensor.h"
#include "fpsensor_auth_commands.h"
#include "fpsensor_auth_crypto.h"
#include "fpsensor_crypto.h"
#include "fpsensor_state.h"
#include "fpsensor_utils.h"
#include "openssl/mem.h"
#include "openssl/rand.h"
#include "scoped_fast_cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

/* Store the intermediate encrypted data for transfer & reuse purpose.*/
/* The data will be copied into fp_enc_buffer after commit. */
static std::array<std::array<uint8_t, FP_ALGORITHM_ENCRYPTED_TEMPLATE_SIZE>,
		  FP_MAX_FINGER_COUNT>
	fp_xfer_buffer;

/* The GSC pairing key. */
static std::array<uint8_t, FP_PAIRING_KEY_LEN> pairing_key;

/* The auth nonce for GSC session key. */
std::array<uint8_t, FP_CK_AUTH_NONCE_LEN> auth_nonce;

enum ec_error_list check_context_cleared()
{
	for (uint32_t partial : user_id)
		if (partial != 0)
			return EC_ERROR_ACCESS_DENIED;
	for (uint8_t partial : auth_nonce)
		if (partial != 0)
			return EC_ERROR_ACCESS_DENIED;
	if (templ_valid != 0)
		return EC_ERROR_ACCESS_DENIED;
	if (templ_dirty != 0)
		return EC_ERROR_ACCESS_DENIED;
	if (positive_match_secret_state.template_matched != FP_NO_SUCH_TEMPLATE)
		return EC_ERROR_ACCESS_DENIED;
	return EC_SUCCESS;
}

static enum ec_status
fp_command_establish_pairing_key_keygen(struct host_cmd_handler_args *args)
{
	auto *r = static_cast<ec_response_fp_establish_pairing_key_keygen *>(
		args->response);

	ScopedFastCpu fast_cpu;

	bssl::UniquePtr<EC_KEY> ecdh_key = generate_elliptic_curve_key();
	if (ecdh_key == nullptr) {
		return EC_RES_UNAVAILABLE;
	}

	std::optional<fp_encrypted_private_key> encrypted_private_key =
		create_encrypted_private_key(*ecdh_key,
					     FP_AES_KEY_ENC_METADATA_VERSION);
	if (!encrypted_private_key.has_value()) {
		CPRINTS("pairing_keygen: Failed to fill response encrypted private key");
		return EC_RES_UNAVAILABLE;
	}

	r->encrypted_private_key = std::move(encrypted_private_key).value();

	std::optional<fp_elliptic_curve_public_key> pubkey =
		create_pubkey_from_ec_key(*ecdh_key);
	if (!pubkey.has_value()) {
		return EC_RES_UNAVAILABLE;
	}

	r->pubkey = std::move(pubkey).value();

	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_ESTABLISH_PAIRING_KEY_KEYGEN,
		     fp_command_establish_pairing_key_keygen, EC_VER_MASK(0));

static enum ec_status
fp_command_establish_pairing_key_wrap(struct host_cmd_handler_args *args)
{
	const auto *params =
		static_cast<const ec_params_fp_establish_pairing_key_wrap *>(
			args->params);
	auto *r = static_cast<ec_response_fp_establish_pairing_key_wrap *>(
		args->response);

	ScopedFastCpu fast_cpu;

	bssl::UniquePtr<EC_KEY> private_key =
		decrypt_private_key(params->encrypted_private_key);
	if (private_key == nullptr) {
		return EC_RES_UNAVAILABLE;
	}

	bssl::UniquePtr<EC_KEY> public_key =
		create_ec_key_from_pubkey(params->peers_pubkey);
	if (public_key == nullptr) {
		return EC_RES_UNAVAILABLE;
	}

	enum ec_error_list ret = generate_ecdh_shared_secret(
		*private_key, *public_key, r->encrypted_pairing_key.data,
		sizeof(r->encrypted_pairing_key.data));
	if (ret != EC_SUCCESS) {
		return EC_RES_UNAVAILABLE;
	}

	ret = encrypt_data_in_place(FP_AES_KEY_ENC_METADATA_VERSION,
				    r->encrypted_pairing_key.info,
				    r->encrypted_pairing_key.data,
				    sizeof(r->encrypted_pairing_key.data));
	if (ret != EC_SUCCESS) {
		return EC_RES_UNAVAILABLE;
	}

	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_ESTABLISH_PAIRING_KEY_WRAP,
		     fp_command_establish_pairing_key_wrap, EC_VER_MASK(0));

static enum ec_status
fp_command_load_pairing_key(struct host_cmd_handler_args *args)
{
	const auto *params = static_cast<const ec_params_fp_load_pairing_key *>(
		args->params);

	ScopedFastCpu fast_cpu;

	/* If the context is not cleared, reject this request to prevent leaking
	 * the existing template. */
	enum ec_error_list ret = check_context_cleared();
	if (ret != EC_SUCCESS) {
		CPRINTS("load_pairing_key: Context is not clean");
		return EC_RES_ACCESS_DENIED;
	}

	if (fp_encryption_status & FP_CONTEXT_STATUS_NONCE_CONTEXT_SET) {
		CPRINTS("load_pairing_key: In an nonce context");
		return EC_RES_ACCESS_DENIED;
	}

	ret = decrypt_data(params->encrypted_pairing_key.info,
			   params->encrypted_pairing_key.data,
			   sizeof(params->encrypted_pairing_key.data),
			   pairing_key.data(), pairing_key.size());
	if (ret != EC_SUCCESS) {
		CPRINTS("load_pairing_key: Failed to decrypt pairing key");
		return EC_RES_UNAVAILABLE;
	}

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_LOAD_PAIRING_KEY, fp_command_load_pairing_key,
		     EC_VER_MASK(0));

static enum ec_status
fp_command_generate_nonce(struct host_cmd_handler_args *args)
{
	auto *r = static_cast<ec_response_fp_generate_nonce *>(args->response);

	ScopedFastCpu fast_cpu;

	if (fp_encryption_status & FP_CONTEXT_STATUS_NONCE_CONTEXT_SET) {
		/* If the context is not cleared, reject this request to prevent
		 * leaking the existing template. */
		enum ec_error_list ret = check_context_cleared();
		if (ret != EC_SUCCESS) {
			CPRINTS("load_pairing_key: Context is not clean");
			return EC_RES_ACCESS_DENIED;
		}
	}

	RAND_bytes(auth_nonce.data(), auth_nonce.size());

	std::copy(auth_nonce.begin(), auth_nonce.end(), r->nonce);

	fp_encryption_status |= FP_CONTEXT_AUTH_NONCE_SET;

	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_GENERATE_NONCE, fp_command_generate_nonce,
		     EC_VER_MASK(0));

static enum ec_status
fp_command_nonce_context(struct host_cmd_handler_args *args)
{
	const auto *p =
		static_cast<const ec_params_fp_nonce_context *>(args->params);

	if (!(fp_encryption_status & FP_CONTEXT_AUTH_NONCE_SET)) {
		CPRINTS("No existing auth nonce");
		return EC_RES_ACCESS_DENIED;
	}

	ScopedFastCpu fast_cpu;

	std::array<uint8_t, SHA256_DIGEST_SIZE> gsc_session_key;
	enum ec_error_list ret = generate_gsc_session_key(
		auth_nonce.data(), auth_nonce.size(), p->gsc_nonce,
		sizeof(p->gsc_nonce), pairing_key.data(), pairing_key.size(),
		gsc_session_key.data(), gsc_session_key.size());

	if (ret != EC_SUCCESS) {
		return EC_RES_INVALID_PARAM;
	}

	static_assert(sizeof(user_id) == sizeof(p->enc_user_id));
	std::array<uint8_t, sizeof(user_id)> raw_user_id;
	std::copy(std::begin(p->enc_user_id), std::end(p->enc_user_id),
		  raw_user_id.data());

	ret = decrypt_data_with_gsc_session_key_in_place(
		gsc_session_key.data(), gsc_session_key.size(),
		p->enc_user_id_iv, sizeof(p->enc_user_id_iv),
		raw_user_id.data(), raw_user_id.size());

	if (ret != EC_SUCCESS) {
		return EC_RES_ERROR;
	}

	/* Set the user_id. */
	std::copy(raw_user_id.begin(), raw_user_id.end(),
		  reinterpret_cast<uint8_t *>(user_id));

	fp_encryption_status &= FP_ENC_STATUS_SEED_SET;
	fp_encryption_status |= FP_CONTEXT_STATUS_NONCE_CONTEXT_SET;
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_NONCE_CONTEXT, fp_command_nonce_context,
		     EC_VER_MASK(0));

static enum ec_status
fp_command_read_match_secret_with_pubkey(struct host_cmd_handler_args *args)
{
	const auto *params =
		static_cast<const ec_params_fp_read_match_secret_with_pubkey *>(
			args->params);
	auto *response =
		static_cast<ec_response_fp_read_match_secret_with_pubkey *>(
			args->response);
	int8_t fgr = params->fgr;

	ScopedFastCpu fast_cpu;

	static_assert(sizeof(response->enc_secret) ==
		      FP_POSITIVE_MATCH_SECRET_BYTES);

	CleanseWrapper<std::array<uint8_t, FP_POSITIVE_MATCH_SECRET_BYTES> >
		secret;

	enum ec_status status = fp_read_match_secret(fgr, secret.data());
	if (status != EC_RES_SUCCESS) {
		return status;
	}

	enum ec_error_list ret = encrypt_data_with_ecdh_key_in_place(
		params->pubkey, secret.data(), secret.size(), response->iv,
		sizeof(response->iv), response->pubkey);

	if (ret != EC_SUCCESS) {
		return EC_RES_UNAVAILABLE;
	}

	std::copy(secret.begin(), secret.end(), response->enc_secret);

	args->response_size = sizeof(*response);

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_READ_MATCH_SECRET_WITH_PUBKEY,
		     fp_command_read_match_secret_with_pubkey, EC_VER_MASK(0));

static enum ec_error_list preload_template(const uint8_t *data, uint32_t size,
					   uint32_t offset, uint16_t idx,
					   bool xfer_complete)
{
	/* Can we store one more template ? */
	if (idx >= FP_MAX_FINGER_COUNT)
		return EC_ERROR_OVERFLOW;

	enum ec_error_list ret = validate_fp_buffer_offset(
		fp_xfer_buffer[0].size(), offset, size);
	if (ret != EC_SUCCESS)
		return ret;

	std::copy(data, data + size, fp_xfer_buffer[idx].data() + offset);

	if (xfer_complete) {
		std::copy(fp_xfer_buffer[idx].begin(),
			  fp_xfer_buffer[idx].end(), fp_enc_buffer);
	}

	return EC_SUCCESS;
}

static enum ec_status
fp_command_preload_template(struct host_cmd_handler_args *args)
{
	const auto *params = static_cast<const ec_params_fp_preload_template *>(
		args->params);

	ScopedFastCpu fast_cpu;

	uint32_t size = params->size & ~FP_TEMPLATE_COMMIT;
	bool xfer_complete = params->size & FP_TEMPLATE_COMMIT;

	if (args->params_size !=
	    size + offsetof(ec_params_fp_preload_template, data))
		return EC_RES_REQUEST_TRUNCATED;

	enum ec_error_list ret = preload_template(
		params->data, size, params->offset, params->fgr, xfer_complete);
	if (ret == EC_ERROR_OVERFLOW)
		return EC_RES_OVERFLOW;
	else if (ret != EC_SUCCESS)
		return EC_RES_INVALID_PARAM;

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_PRELOAD_TEMPLATE, fp_command_preload_template,
		     EC_VER_MASK(0));
