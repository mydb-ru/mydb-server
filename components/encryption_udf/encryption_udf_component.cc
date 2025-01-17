/* Copyright (c) 2022 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <array>
#include <bitset>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/algorithm/string/predicate.hpp>

#include <boost/lexical_cast/try_lexical_convert.hpp>

#include <boost/preprocessor/stringize.hpp>

#include <mysql/components/component_implementation.h>

#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysql/components/services/mysql_runtime_error.h>
#include <mysql/components/services/udf_metadata.h>
#include <mysql/components/services/udf_registration.h>

#include <mysqlpp/udf_context_charset_extension.hpp>
#include <mysqlpp/udf_registration.hpp>
#include <mysqlpp/udf_wrappers.hpp>

#include <opensslpp/core_error.hpp>
#include <opensslpp/dh_compute_operations.hpp>
#include <opensslpp/dh_key.hpp>
#include <opensslpp/dh_padding.hpp>
#include <opensslpp/digest_operations.hpp>
#include <opensslpp/dsa_key.hpp>
#include <opensslpp/dsa_sign_verify_operations.hpp>
#include <opensslpp/evp_pkey.hpp>
#include <opensslpp/evp_pkey_algorithm.hpp>
#include <opensslpp/evp_pkey_sign_verify_operations.hpp>
#include <opensslpp/evp_pkey_signature_padding.hpp>
#include <opensslpp/operation_cancelled_error.hpp>
#include <opensslpp/rsa_encrypt_decrypt_operations.hpp>
#include <opensslpp/rsa_encryption_padding.hpp>
#include <opensslpp/rsa_key.hpp>

#include "server_helpers.h"

// defined as a macro because needed both raw and stringized
#define CURRENT_COMPONENT_NAME encryption_udf
#define CURRENT_COMPONENT_NAME_STR BOOST_PP_STRINGIZE(CURRENT_COMPONENT_NAME)

REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
REQUIRES_SERVICE_PLACEHOLDER(udf_registration);
REQUIRES_SERVICE_PLACEHOLDER(mysql_udf_metadata);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);

namespace {

opensslpp::evp_pkey_algorithm get_and_validate_algorithm_id_by_label(
    std::string_view algorithm) {
  if (algorithm.data() == nullptr)
    throw std::invalid_argument("Algorithm cannot be NULL");

  if (boost::iequals(algorithm, "rsa"))
    return opensslpp::evp_pkey_algorithm::rsa;
  if (boost::iequals(algorithm, "dsa"))
    return opensslpp::evp_pkey_algorithm::dsa;
  if (boost::iequals(algorithm, "dh")) return opensslpp::evp_pkey_algorithm::dh;

  throw std::invalid_argument("Invalid algorithm specified");
}

opensslpp::rsa_encryption_padding
get_and_validate_rsa_encryption_padding_by_label(std::string_view padding) {
  if (padding.data() == nullptr)
    throw std::invalid_argument("RSA encryption padding scheme cannot be NULL");

  if (boost::iequals(padding, "no"))
    return opensslpp::rsa_encryption_padding::no;
  if (boost::iequals(padding, "pkcs1"))
    return opensslpp::rsa_encryption_padding::pkcs1;
  if (boost::iequals(padding, "oaep"))
    return opensslpp::rsa_encryption_padding::pkcs1_oaep;

  throw std::invalid_argument(
      "Invalid RSA encryption padding scheme specified");
}

opensslpp::evp_pkey_signature_padding
get_and_validate_evp_pkey_signature_padding_by_label(std::string_view padding) {
  if (padding.data() == nullptr)
    throw std::invalid_argument("RSA signature padding scheme cannot be NULL");

  if (boost::iequals(padding, "pkcs1"))
    return opensslpp::evp_pkey_signature_padding::rsa_pkcs1;
  if (boost::iequals(padding, "pkcs1_pss"))
    return opensslpp::evp_pkey_signature_padding::rsa_pkcs1_pss;

  throw std::invalid_argument("Invalid RSA signature padding scheme specified");
}

enum class threshold_index_type { rsa, dsa, dh, delimiter };
constexpr std::size_t number_of_thresholds =
    static_cast<std::size_t>(threshold_index_type::delimiter);

uint bits_threshold_values[number_of_thresholds] = {};
bool legacy_padding_scheme_value{false};

struct threshold_definition {
  std::size_t min;
  std::size_t max;
  const char *var_name;
  const char *var_description;
};

// DSA max key length must be <= OPENSSL_DSA_MAX_MODULUS_BITS (10000)
// and be a multiple of 64, therefore 9984
constexpr std::size_t aligned_max_dsa_bits = 10000U / 64U * 64U;

constexpr std::array<threshold_definition, number_of_thresholds> thresholds = {
    threshold_definition{
        1024U, 16384U, "rsa_bits_threshold",
        "Maximum RSA key length in bits for create_asymmetric_priv_key()"},
    threshold_definition{
        1024U, aligned_max_dsa_bits, "dsa_bits_threshold",
        "Maximum DSA key length in bits for create_asymmetric_priv_key()"},
    threshold_definition{
        1024U, 10000U, "dh_bits_threshold",
        "Maximum DH key length in bits for create_dh_parameters()"}};
constexpr char legacy_padding_scheme_variable_name[] = "legacy_padding_scheme";
constexpr char legacy_padding_scheme_variable_description[] =
    "Enable legacy padding scheme for RSA sign/verify and encrypt/decrypt";

using udf_int_arg_raw_type = mysqlpp::udf_arg_t<INT_RESULT>::value_type;
bool check_if_bits_in_range(udf_int_arg_raw_type value,
                            threshold_index_type threshold_index) noexcept {
  const auto casted_threshold_index{static_cast<std::size_t>(threshold_index)};
  const auto &threshold = thresholds[casted_threshold_index];

  if (value < static_cast<udf_int_arg_raw_type>(threshold.min)) return false;

  std::size_t max_value = bits_threshold_values[casted_threshold_index];

  if (value > static_cast<udf_int_arg_raw_type>(max_value)) return false;
  return true;
}
bool check_if_legacy_padding_scheme() noexcept {
  return legacy_padding_scheme_value;
}

opensslpp::key_generation_cancellation_callback create_cancellation_callback() {
  THD *local_thd = nullptr;
  if (mysql_service_mysql_current_thread_reader->get(&local_thd) != 0 ||
      local_thd == nullptr)
    throw std::invalid_argument("Cannot extract current THD");

  return [local_thd]() noexcept -> bool { return is_thd_killed(local_thd); };
}

constexpr char ascii_charset_name[]{"ascii"};
constexpr char binary_charset_name[]{"binary"};

// CREATE_ASYMMETRIC_PRIV_KEY(@algorithm, {@key_len|@dh_parameters})
// This functions generates a private key using the given algorithm
// (@algorithm) and key length (@key_len) or Diffie-Hellman
// secret(@dh_parameters), and returns the key as a binary string in PEM format.
// If key generation fails, the result is NULL.
class create_asymmetric_priv_key_impl {
 public:
  create_asymmetric_priv_key_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 2)
      throw std::invalid_argument("Function requires exactly two arguments");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    charset_ext.set_return_value_charset(ctx, ascii_charset_name);

    // arg0 - @algorithm
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 0, ascii_charset_name);

    // arg1 - @key_len|@dh_parameters
    ctx.mark_arg_nullable(1, false);
    ctx.set_arg_type(1, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 1, ascii_charset_name);
  }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<STRING_RESULT> create_asymmetric_priv_key_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto algorithm_sv = ctx.get_arg<STRING_RESULT>(0);
  auto algorithm_id = get_and_validate_algorithm_id_by_label(algorithm_sv);
  auto length_or_dh_parameters_sv = ctx.get_arg<STRING_RESULT>(1);

  std::string pem;
  if (algorithm_id == opensslpp::evp_pkey_algorithm::dh) {
    auto key =
        opensslpp::dh_key::import_parameters_pem(length_or_dh_parameters_sv);
    key.promote_to_key();
    pem = opensslpp::dh_key::export_private_pem(key);
  } else {
    std::uint32_t length = 0;
    if (!boost::conversion::try_lexical_convert(length_or_dh_parameters_sv,
                                                length))
      throw std::invalid_argument("Key length is not a numeric value");

    if (algorithm_id == opensslpp::evp_pkey_algorithm::rsa) {
      if (!check_if_bits_in_range(length, threshold_index_type::rsa))
        throw std::invalid_argument("Invalid RSA key length specified");

      opensslpp::evp_pkey key;
      try {
        key = opensslpp::evp_pkey::generate(algorithm_id, length,
                                            create_cancellation_callback());
      } catch (const opensslpp::operation_cancelled_error &e) {
        throw mysqlpp::udf_exception{e.what(), ER_QUERY_INTERRUPTED};
      }
      pem = opensslpp::evp_pkey::export_private_pem(key);
    } else if (algorithm_id == opensslpp::evp_pkey_algorithm::dsa) {
      if (!check_if_bits_in_range(length, threshold_index_type::dsa))
        throw std::invalid_argument("Invalid DSA key length specified");
      opensslpp::dsa_key key;
      try {
        key = opensslpp::dsa_key::generate_parameters(
            length, create_cancellation_callback());
      } catch (const opensslpp::operation_cancelled_error &e) {
        throw mysqlpp::udf_exception{e.what(), ER_QUERY_INTERRUPTED};
      }
      key.promote_to_key();
      pem = opensslpp::dsa_key::export_private_pem(key);
    }
  }

  return {std::move(pem)};
}

// CREATE_ASYMMETRIC_PUB_KEY(@algorithm, @priv_key_str)
// Derives a public key from the given private key using the given algorithm,
// and returns the key as a binary string in PEM format.
// If key derivation fails, the result is NULL.
class create_asymmetric_pub_key_impl {
 public:
  create_asymmetric_pub_key_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 2)
      throw std::invalid_argument("Function requires exactly two arguments");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    charset_ext.set_return_value_charset(ctx, ascii_charset_name);

    // arg0 - @algorithm
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 0, ascii_charset_name);

    // arg1 - @priv_key_str
    ctx.mark_arg_nullable(1, false);
    ctx.set_arg_type(1, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 1, ascii_charset_name);
  }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<STRING_RESULT> create_asymmetric_pub_key_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto algorithm_sv = ctx.get_arg<STRING_RESULT>(0);
  auto algorithm_id = get_and_validate_algorithm_id_by_label(algorithm_sv);
  auto priv_key_pem_sv = ctx.get_arg<STRING_RESULT>(1);

  std::string pem;
  if (algorithm_id == opensslpp::evp_pkey_algorithm::rsa) {
    auto priv_key = opensslpp::evp_pkey::import_private_pem(priv_key_pem_sv);
    pem = opensslpp::evp_pkey::export_public_pem(priv_key);
  } else if (algorithm_id == opensslpp::evp_pkey_algorithm::dsa) {
    auto priv_key = opensslpp::dsa_key::import_private_pem(priv_key_pem_sv);
    pem = opensslpp::dsa_key::export_public_pem(priv_key);
  } else if (algorithm_id == opensslpp::evp_pkey_algorithm::dh) {
    auto priv_key = opensslpp::dh_key::import_private_pem(priv_key_pem_sv);
    pem = opensslpp::dh_key::export_public_pem(priv_key);
  }
  return {std::move(pem)};
}

// ASYMMETRIC_ENCRYPT(@algorithm, @str, @key_str)
// Encrypts a string using the given algorithm and key string, and returns
// the resulting ciphertext as a binary string.
// If encryption fails, the result is NULL.
class asymmetric_encrypt_impl {
 public:
  asymmetric_encrypt_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() < 3 || ctx.get_number_of_args() > 4)
      throw std::invalid_argument("Function requires three or four arguments");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    charset_ext.set_return_value_charset(ctx, binary_charset_name);

    // arg0 - @algorithm
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 0, ascii_charset_name);

    // arg1 - @str
    ctx.mark_arg_nullable(1, false);
    ctx.set_arg_type(1, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 1, binary_charset_name);

    // arg2 - @key_str
    ctx.mark_arg_nullable(2, false);
    ctx.set_arg_type(2, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 2, ascii_charset_name);

    // optional arg3 - @padding
    if (ctx.get_number_of_args() == 4) {
      ctx.mark_arg_nullable(3, false);
      ctx.set_arg_type(3, STRING_RESULT);
      charset_ext.set_arg_value_charset(ctx, 3, ascii_charset_name);
    }
  }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<STRING_RESULT> asymmetric_encrypt_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto algorithm_sv = ctx.get_arg<STRING_RESULT>(0);
  auto algorithm_id = get_and_validate_algorithm_id_by_label(algorithm_sv);
  if (algorithm_id != opensslpp::evp_pkey_algorithm::rsa)
    throw std::invalid_argument("Invalid algorithm specified");

  auto message_sv = ctx.get_arg<STRING_RESULT>(1);
  auto key_pem_sv = ctx.get_arg<STRING_RESULT>(2);

  opensslpp::rsa_encryption_padding padding{
      check_if_legacy_padding_scheme()
          ? opensslpp::rsa_encryption_padding::pkcs1
          : opensslpp::rsa_encryption_padding::pkcs1_oaep};
  if (ctx.get_number_of_args() == 4) {
    auto padding_sv = ctx.get_arg<STRING_RESULT>(3);
    padding = get_and_validate_rsa_encryption_padding_by_label(padding_sv);
  }

  opensslpp::rsa_key key;
  try {
    key = opensslpp::rsa_key::import_private_pem(key_pem_sv);
  } catch (const opensslpp::core_error &) {
    key = opensslpp::rsa_key::import_public_pem(key_pem_sv);
  }

  return {
      key.is_private()
          ? opensslpp::encrypt_with_rsa_private_key(message_sv, key, padding)
          : opensslpp::encrypt_with_rsa_public_key(message_sv, key, padding)};
}

// ASYMMETRIC_DECRYPT(@algorithm, @crypt_str, @key_str)
// Decrypts an encrypted string using the given algorithm and key string, and
// returns the resulting plaintext as a binary string. If decryption fails, the
// result is NULL.
class asymmetric_decrypt_impl {
 public:
  asymmetric_decrypt_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() < 3 || ctx.get_number_of_args() > 4)
      throw std::invalid_argument("Function requires three or four arguments");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    charset_ext.set_return_value_charset(ctx, binary_charset_name);

    // arg0 - @algorithm
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 0, ascii_charset_name);

    // arg1 - @crypt_str
    ctx.mark_arg_nullable(1, false);
    ctx.set_arg_type(1, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 1, binary_charset_name);

    // arg2 - @key_str
    ctx.mark_arg_nullable(2, false);
    ctx.set_arg_type(2, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 2, ascii_charset_name);

    // optional arg3 - @padding
    if (ctx.get_number_of_args() == 4) {
      ctx.mark_arg_nullable(3, false);
      ctx.set_arg_type(3, STRING_RESULT);
      charset_ext.set_arg_value_charset(ctx, 3, ascii_charset_name);
    }
  }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<STRING_RESULT> asymmetric_decrypt_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto algorithm_sv = ctx.get_arg<STRING_RESULT>(0);
  auto algorithm_id = get_and_validate_algorithm_id_by_label(algorithm_sv);
  if (algorithm_id != opensslpp::evp_pkey_algorithm::rsa)
    throw std::invalid_argument("Invalid algorithm specified");

  auto message_sv = ctx.get_arg<STRING_RESULT>(1);
  auto key_pem_sv = ctx.get_arg<STRING_RESULT>(2);

  opensslpp::rsa_encryption_padding padding{
      check_if_legacy_padding_scheme()
          ? opensslpp::rsa_encryption_padding::pkcs1
          : opensslpp::rsa_encryption_padding::pkcs1_oaep};
  if (ctx.get_number_of_args() == 4) {
    auto padding_sv = ctx.get_arg<STRING_RESULT>(3);
    padding = get_and_validate_rsa_encryption_padding_by_label(padding_sv);
  }

  opensslpp::rsa_key key;
  try {
    key = opensslpp::rsa_key::import_private_pem(key_pem_sv);
  } catch (const opensslpp::core_error &) {
    key = opensslpp::rsa_key::import_public_pem(key_pem_sv);
  }

  return {
      key.is_private()
          ? opensslpp::decrypt_with_rsa_private_key(message_sv, key, padding)
          : opensslpp::decrypt_with_rsa_public_key(message_sv, key, padding)};
}

// CREATE_DIGEST(@digest_type, @str)
// Creates a digest from the given string using the given digest type, and
// returns the digest as a binary string.
// If digest generation fails, the result is NULL.
// Supported @digest_type values: 'SHA224', 'SHA256', 'SHA384', 'SHA512'
class create_digest_impl {
 public:
  create_digest_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 2)
      throw std::invalid_argument("Function requires exactly two arguments");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    charset_ext.set_return_value_charset(ctx, binary_charset_name);

    // arg0 - @digest_type
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 0, ascii_charset_name);

    // arg1 - @str
    ctx.mark_arg_nullable(1, false);
    ctx.set_arg_type(1, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 1, binary_charset_name);
  }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<STRING_RESULT> create_digest_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto digest_type_sv = ctx.get_arg<STRING_RESULT>(0);
  auto digest_type = static_cast<std::string>(digest_type_sv);

  auto message_sv = ctx.get_arg<STRING_RESULT>(1);

  return {opensslpp::calculate_digest(digest_type, message_sv)};
}

// ASYMMETRIC_SIGN(@algorithm, @digest_str, @priv_key_str, @digest_type)
// Signs a digest string using a private key string, and returns the signature
// as a binary string.
// If signing fails, the result is NULL.
// @digest_str is the digest string. It can be generated by calling
// CREATE_DIGEST().
// @digest_type indicates the digest algorithm used to generate the digest
// string.
// @priv_key_str is the private key string to use for signing the digest string.
// It must be a valid key string in PEM format.
// @algorithm indicates the encryption algorithm used to create the key.
// Supported @algorithm values: 'RSA', 'DSA'
// Supported @digest_type values: 'SHA224', 'SHA256', 'SHA384', 'SHA512'
class asymmetric_sign_impl {
 public:
  asymmetric_sign_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() < 4 || ctx.get_number_of_args() > 5)
      throw std::invalid_argument("Function requires four or five arguments");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    charset_ext.set_return_value_charset(ctx, binary_charset_name);

    // arg0 - @algorithm
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 0, ascii_charset_name);

    // arg1 - @digest_str
    ctx.mark_arg_nullable(1, false);
    ctx.set_arg_type(1, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 1, binary_charset_name);

    // arg2 - @priv_key_str
    ctx.mark_arg_nullable(2, false);
    ctx.set_arg_type(2, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 2, ascii_charset_name);

    // arg3 - @digest_type
    ctx.mark_arg_nullable(3, false);
    ctx.set_arg_type(3, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 3, ascii_charset_name);

    // optional arg4 - @padding
    if (ctx.get_number_of_args() == 5) {
      ctx.mark_arg_nullable(4, false);
      ctx.set_arg_type(4, STRING_RESULT);
      charset_ext.set_arg_value_charset(ctx, 4, ascii_charset_name);
    }
  }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<STRING_RESULT> asymmetric_sign_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto algorithm_sv = ctx.get_arg<STRING_RESULT>(0);
  auto algorithm_id = get_and_validate_algorithm_id_by_label(algorithm_sv);
  if (algorithm_id != opensslpp::evp_pkey_algorithm::rsa &&
      algorithm_id != opensslpp::evp_pkey_algorithm::dsa)
    throw std::invalid_argument("Invalid algorithm specified");

  opensslpp::evp_pkey_signature_padding signature_padding{
      check_if_legacy_padding_scheme()
          ? opensslpp::evp_pkey_signature_padding::rsa_pkcs1
          : opensslpp::evp_pkey_signature_padding::rsa_pkcs1_pss};
  if (ctx.get_number_of_args() == 5) {
    if (algorithm_id != opensslpp::evp_pkey_algorithm::rsa)
      throw std::invalid_argument(
          "Signature padding scheme can only be specified for the RSA "
          "algorithm");
    auto signature_padding_sv = ctx.get_arg<STRING_RESULT>(4);
    signature_padding = get_and_validate_evp_pkey_signature_padding_by_label(
        signature_padding_sv);
  }

  auto message_digest_sv = ctx.get_arg<STRING_RESULT>(1);
  auto private_key_pem_sv = ctx.get_arg<STRING_RESULT>(2);
  auto digest_type_sv = ctx.get_arg<STRING_RESULT>(3);
  auto digest_type = static_cast<std::string>(digest_type_sv);

  std::string signature;
  if (algorithm_id == opensslpp::evp_pkey_algorithm::rsa) {
    auto private_key =
        opensslpp::evp_pkey::import_private_pem(private_key_pem_sv);
    signature = opensslpp::sign_with_private_evp_pkey(
        digest_type, message_digest_sv, private_key, signature_padding);
  } else if (algorithm_id == opensslpp::evp_pkey_algorithm::dsa) {
    auto private_key =
        opensslpp::dsa_key::import_private_pem(private_key_pem_sv);
    signature = opensslpp::sign_with_dsa_private_key(
        digest_type, message_digest_sv, private_key);
  }
  return {std::move(signature)};
}

// ASYMMETRIC_VERIFY(@algorithm, @digest_str, @sig_str, @pub_key_str,
// @digest_type) Verifies whether the signature string matches the digest
// string, and returns 1 or 0 to indicate whether verification succeeded or
// failed.
// @digest_str is the digest string. It can be generated by calling
// CREATE_DIGEST().
// @digest_type indicates the digest algorithm used to generate the digest
// string.
// @sig_str is the signature string. It can be generated by calling
// ASYMMETRIC_SIGN().
// @pub_key_str is the public key string of the signer. It corresponds to the
// private key passed to ASYMMETRIC_SIGN() to generate the signature string
// and must be a valid key string in PEM format.
// @algorithm indicates the encryption algorithm used to create the key.
// Supported algorithm values: 'RSA', 'DSA'
// Supported digest_type values: 'SHA224', 'SHA256', 'SHA384', 'SHA512'
class asymmetric_verify_impl {
 public:
  asymmetric_verify_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() < 5 || ctx.get_number_of_args() > 6)
      throw std::invalid_argument("Function requires five or six arguments");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    // return value charset is not set here as its type is INT_RESULT

    // arg0 - @algorithm
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 0, ascii_charset_name);

    // arg1 - @digest_str
    ctx.mark_arg_nullable(1, false);
    ctx.set_arg_type(1, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 1, binary_charset_name);

    // arg2 - @sig_str
    ctx.mark_arg_nullable(2, false);
    ctx.set_arg_type(2, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 2, binary_charset_name);

    // arg3 - @pub_key_str
    ctx.mark_arg_nullable(3, false);
    ctx.set_arg_type(3, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 3, ascii_charset_name);

    // arg4 - @digest_type
    ctx.mark_arg_nullable(4, false);
    ctx.set_arg_type(4, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 4, ascii_charset_name);

    // optional arg5 - @padding
    if (ctx.get_number_of_args() == 6) {
      ctx.mark_arg_nullable(5, false);
      ctx.set_arg_type(5, STRING_RESULT);
      charset_ext.set_arg_value_charset(ctx, 5, ascii_charset_name);
    }
  }

  mysqlpp::udf_result_t<INT_RESULT> calculate(const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<INT_RESULT> asymmetric_verify_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto algorithm_sv = ctx.get_arg<STRING_RESULT>(0);
  auto algorithm_id = get_and_validate_algorithm_id_by_label(algorithm_sv);
  if (algorithm_id != opensslpp::evp_pkey_algorithm::rsa &&
      algorithm_id != opensslpp::evp_pkey_algorithm::dsa)
    throw std::invalid_argument("Invalid algorithm specified");

  opensslpp::evp_pkey_signature_padding signature_padding{
      check_if_legacy_padding_scheme()
          ? opensslpp::evp_pkey_signature_padding::rsa_pkcs1
          : opensslpp::evp_pkey_signature_padding::rsa_pkcs1_pss};
  if (ctx.get_number_of_args() == 6) {
    if (algorithm_id != opensslpp::evp_pkey_algorithm::rsa)
      throw std::invalid_argument(
          "Signature padding scheme can only be specified for the RSA "
          "algorithm");
    auto signature_padding_sv = ctx.get_arg<STRING_RESULT>(5);
    signature_padding = get_and_validate_evp_pkey_signature_padding_by_label(
        signature_padding_sv);
  }

  auto message_digest_sv = ctx.get_arg<STRING_RESULT>(1);
  auto signature_sv = ctx.get_arg<STRING_RESULT>(2);
  auto public_key_pem_sv = ctx.get_arg<STRING_RESULT>(3);
  auto digest_type_sv = ctx.get_arg<STRING_RESULT>(4);
  auto digest_type = static_cast<std::string>(digest_type_sv);

  bool verification_result = false;
  if (algorithm_id == opensslpp::evp_pkey_algorithm::rsa) {
    auto public_key = opensslpp::evp_pkey::import_public_pem(public_key_pem_sv);
    verification_result = opensslpp::verify_with_public_evp_pkey(
        digest_type, message_digest_sv, signature_sv, public_key,
        signature_padding);
  } else if (algorithm_id == opensslpp::evp_pkey_algorithm::dsa) {
    auto public_key = opensslpp::dsa_key::import_public_pem(public_key_pem_sv);
    verification_result = opensslpp::verify_with_dsa_public_key(
        digest_type, message_digest_sv, signature_sv, public_key);
  }
  return {verification_result ? 1LL : 0LL};
}

// CREATE_DH_PARAMETERS(@key_len)
// Creates parameters for generating a DH private/public key pair and returns
// them in PEM format. The parameters can be passed to
// CREATE_ASYMMETRIC_PRIV_KEY(). If secret generation fails, the result is null.
// Supported @key_len values: The minimum and maximum key lengths in bits are
// 1,024 and 10,000. These key-length limits are constraints imposed by OpenSSL.
class create_dh_parameters_impl {
 public:
  create_dh_parameters_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 1)
      throw std::invalid_argument("Function requires exactly one argument");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    charset_ext.set_return_value_charset(ctx, ascii_charset_name);

    // arg0 - @key_len
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, INT_RESULT);
    // argument charset is not set here as its type is INT_RESULT
  }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<STRING_RESULT> create_dh_parameters_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto optional_length = ctx.get_arg<INT_RESULT>(0);
  if (!optional_length)
    throw std::invalid_argument("Parameters length cannot be NULL");
  auto length = optional_length.value();

  if (!check_if_bits_in_range(length, threshold_index_type::dh))
    throw std::invalid_argument("Invalid DH parameters length specified");

  opensslpp::dh_key key;
  try {
    key = opensslpp::dh_key::generate_parameters(
        length, opensslpp::dh_key::default_generator,
        create_cancellation_callback());
  } catch (const opensslpp::operation_cancelled_error &e) {
    throw mysqlpp::udf_exception{e.what(), ER_QUERY_INTERRUPTED};
  }
  key.promote_to_key();

  return {opensslpp::dh_key::export_parameters_pem(key)};
}

// ASYMMETRIC_DERIVE(@pub_key_str, @priv_key_str)
// Derives a symmetric key using the private key of one party and the public
// key of another, and returns the resulting key as a binary string.
// If key derivation fails, the result is NULL.
// @pub_key_str and @priv_key_str must be valid key strings in PEM format.
// They must be created using the DH algorithm.
class asymmetric_derive_impl {
 public:
  asymmetric_derive_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 2)
      throw std::invalid_argument("Function requires exactly two arguments");

    mysqlpp::udf_context_charset_extension charset_ext{
        mysql_service_mysql_udf_metadata};

    // result
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    charset_ext.set_return_value_charset(ctx, binary_charset_name);

    // arg0 - @pub_key_str
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 0, ascii_charset_name);

    // arg1 - @priv_key_str
    ctx.mark_arg_nullable(1, false);
    ctx.set_arg_type(1, STRING_RESULT);
    charset_ext.set_arg_value_charset(ctx, 1, ascii_charset_name);
  }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx);
};

mysqlpp::udf_result_t<STRING_RESULT> asymmetric_derive_impl::calculate(
    const mysqlpp::udf_context &ctx) {
  auto public_key_pem_sv = ctx.get_arg<STRING_RESULT>(0);
  auto public_key = opensslpp::dh_key::import_public_pem(public_key_pem_sv);

  auto private_key_pem_sv = ctx.get_arg<STRING_RESULT>(1);
  auto private_key = opensslpp::dh_key::import_private_pem(private_key_pem_sv);

  return {opensslpp::compute_dh_key(public_key, private_key,
                                    opensslpp::dh_padding::nist_sp800_56a)};
}

}  // end of anonymous namespace

DECLARE_STRING_UDF_AUTO(create_asymmetric_priv_key)
DECLARE_STRING_UDF_AUTO(create_asymmetric_pub_key)
DECLARE_STRING_UDF_AUTO(asymmetric_encrypt)
DECLARE_STRING_UDF_AUTO(asymmetric_decrypt)
DECLARE_STRING_UDF_AUTO(create_digest)
DECLARE_STRING_UDF_AUTO(asymmetric_sign)
DECLARE_INT_UDF_AUTO(asymmetric_verify)
DECLARE_STRING_UDF_AUTO(create_dh_parameters)
DECLARE_STRING_UDF_AUTO(asymmetric_derive)

// TODO: in c++20 (where CTAD works for alias templates) this shoud be changed
// to 'static const udf_info_container known_udfs'
static const std::array known_udfs{
    DECLARE_UDF_INFO_AUTO(create_asymmetric_priv_key),
    DECLARE_UDF_INFO_AUTO(create_asymmetric_pub_key),
    DECLARE_UDF_INFO_AUTO(asymmetric_encrypt),
    DECLARE_UDF_INFO_AUTO(asymmetric_decrypt),
    DECLARE_UDF_INFO_AUTO(create_digest),
    DECLARE_UDF_INFO_AUTO(asymmetric_sign),
    DECLARE_UDF_INFO_AUTO(asymmetric_verify),
    DECLARE_UDF_INFO_AUTO(create_dh_parameters),
    DECLARE_UDF_INFO_AUTO(asymmetric_derive)};

static void encryption_udf_my_error(int error_id, myf flags, ...) {
  va_list args;
  va_start(args, flags);
  mysql_service_mysql_runtime_error->emit(error_id, flags, args);
  va_end(args);
}

using udf_bitset_type =
    mysqlpp::udf_bitset<std::tuple_size_v<decltype(known_udfs)>>;
static udf_bitset_type registered_udfs;

using threshold_bitset_type = std::bitset<number_of_thresholds>;
static threshold_bitset_type registered_thresholds;
static bool legacy_padding_scheme_variable_registered{false};

static mysql_service_status_t component_init() {
  // here, we use a custom error reporting function 'encryption_udf_my_error()'
  // based on the 'mysql_service_mysql_runtime_error' service instead of
  // the standard 'my_error()' from 'mysys' to get rid of the 'mysys'
  // dependency for this component
  mysqlpp::udf_error_reporter::instance() = &encryption_udf_my_error;
  std::size_t index = 0U;

  for (const auto &threshold : thresholds) {
    if (!registered_thresholds.test(index)) {
      INTEGRAL_CHECK_ARG(uint) arg;
      arg.def_val = threshold.max;
      arg.min_val = threshold.min;
      arg.max_val = threshold.max;
      arg.blk_sz = 0;
      if (mysql_service_component_sys_variable_register->register_variable(
              CURRENT_COMPONENT_NAME_STR, threshold.var_name,
              PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_RQCMDARG,
              threshold.var_description, nullptr, nullptr, &arg,
              &bits_threshold_values[index]) == 0)
        registered_thresholds.set(index);
    }
    ++index;
  }
  if (!legacy_padding_scheme_variable_registered) {
    BOOL_CHECK_ARG(bool) arg;
    arg.def_val = false;
    if (mysql_service_component_sys_variable_register->register_variable(
            CURRENT_COMPONENT_NAME_STR, legacy_padding_scheme_variable_name,
            PLUGIN_VAR_BOOL, legacy_padding_scheme_variable_description,
            nullptr, nullptr, &arg, &legacy_padding_scheme_value) == 0) {
      legacy_padding_scheme_variable_registered = true;
    }
  }

  mysqlpp::register_udfs(mysql_service_udf_registration, known_udfs,
                         registered_udfs);
  return registered_udfs.all() && registered_thresholds.all() &&
                 legacy_padding_scheme_variable_registered
             ? 0
             : 1;
}

static mysql_service_status_t component_deinit() {
  mysqlpp::unregister_udfs(mysql_service_udf_registration, known_udfs,
                           registered_udfs);

  std::size_t index = 0;
  for (const auto &threshold : thresholds) {
    if (registered_thresholds.test(index)) {
      if (mysql_service_component_sys_variable_unregister->unregister_variable(
              CURRENT_COMPONENT_NAME_STR, threshold.var_name) == 0) {
        registered_thresholds.reset(index);
      }
    }
    ++index;
  }
  if (legacy_padding_scheme_variable_registered) {
    if (mysql_service_component_sys_variable_unregister->unregister_variable(
            CURRENT_COMPONENT_NAME_STR, legacy_padding_scheme_variable_name) ==
        0) {
      legacy_padding_scheme_variable_registered = false;
    }
  }

  return registered_udfs.none() && registered_thresholds.none() &&
                 !legacy_padding_scheme_variable_registered
             ? 0
             : 1;
}

// clang-format off
BEGIN_COMPONENT_PROVIDES(CURRENT_COMPONENT_NAME)
END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(CURRENT_COMPONENT_NAME)
  REQUIRES_SERVICE(mysql_runtime_error),
  REQUIRES_SERVICE(udf_registration),
  REQUIRES_SERVICE(mysql_udf_metadata),
  REQUIRES_SERVICE(component_sys_variable_register),
  REQUIRES_SERVICE(component_sys_variable_unregister),
  REQUIRES_SERVICE(mysql_current_thread_reader),
END_COMPONENT_REQUIRES();

BEGIN_COMPONENT_METADATA(CURRENT_COMPONENT_NAME)
  METADATA("mysql.author", "Percona Corporation"),
  METADATA("mysql.license", "GPL"),
END_COMPONENT_METADATA();

DECLARE_COMPONENT(CURRENT_COMPONENT_NAME, CURRENT_COMPONENT_NAME_STR)
  component_init,
  component_deinit,
END_DECLARE_COMPONENT();
// clang-format on

DECLARE_LIBRARY_COMPONENTS &COMPONENT_REF(CURRENT_COMPONENT_NAME)
    END_DECLARE_LIBRARY_COMPONENTS
