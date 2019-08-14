/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2019
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  NTP authentication
  */

#include "config.h"

#include "sysincl.h"

#include "keys.h"
#include "logging.h"
#include "memory.h"
#include "ntp_auth.h"
#include "ntp_signd.h"
#include "srcparams.h"
#include "util.h"

/* Structure to hold authentication configuration and state */

struct NAU_Instance_Record {
  NTP_AuthMode mode;            /* Authentication mode of NTP packets */
  uint32_t key_id;              /* Identifier of a symmetric key */
};

/* ================================================== */

static int
generate_symmetric_auth(uint32_t key_id, NTP_Packet *packet, NTP_PacketInfo *info)
{
  int auth_len, max_auth_len;

  /* Truncate long MACs in NTPv4 packets to allow deterministic parsing
     of extension fields (RFC 7822) */
  max_auth_len = (info->version == 4 ? NTP_MAX_V4_MAC_LENGTH : NTP_MAX_MAC_LENGTH) - 4;
  max_auth_len = MIN(max_auth_len, sizeof (NTP_Packet) - info->length - 4);

  auth_len = KEY_GenerateAuth(key_id, (unsigned char *)packet, info->length,
                              (unsigned char *)packet + info->length + 4, max_auth_len);
  if (!auth_len) {
    DEBUG_LOG("Could not generate auth data with key %"PRIu32, key_id);
    return 0;
  }

  *(uint32_t *)((unsigned char *)packet + info->length) = htonl(key_id);
  info->length += 4 + auth_len;

  return 1;
}

/* ================================================== */

static int
check_symmetric_auth(NTP_Packet *packet, NTP_PacketInfo *info)
{
  int trunc_len;

  if (info->auth.mac.length < NTP_MIN_MAC_LENGTH)
    return 0;

  trunc_len = info->version == 4 && info->auth.mac.length <= NTP_MAX_V4_MAC_LENGTH ?
              NTP_MAX_V4_MAC_LENGTH : NTP_MAX_MAC_LENGTH;

  if (!KEY_CheckAuth(info->auth.mac.key_id, (void *)packet, info->auth.mac.start,
                     (unsigned char *)packet + info->auth.mac.start + 4,
                     info->auth.mac.length - 4, trunc_len - 4))
    return 0;

  return 1;
}

/* ================================================== */

static void
adjust_timestamp(NTP_AuthMode mode, uint32_t key_id, struct timespec *ts)
{
  switch (mode) {
    case AUTH_SYMMETRIC:
      ts->tv_nsec += KEY_GetAuthDelay(key_id);
      UTI_NormaliseTimespec(ts);
      break;
    case AUTH_MSSNTP:
      ts->tv_nsec += NSD_GetAuthDelay(key_id);
      UTI_NormaliseTimespec(ts);
    default:
      break;
  }
}

/* ================================================== */

static NAU_Instance
create_instance(NTP_AuthMode mode)
{
  NAU_Instance instance;

  instance = MallocNew(struct NAU_Instance_Record);
  instance->mode = mode;
  instance->key_id = INACTIVE_AUTHKEY;

  assert(sizeof (instance->key_id) == 4);

  return instance;
}

/* ================================================== */

NAU_Instance
NAU_CreateNoneInstance(void)
{
  return create_instance(AUTH_NONE);
}

/* ================================================== */

NAU_Instance
NAU_CreateSymmetricInstance(uint32_t key_id)
{
  NAU_Instance instance = create_instance(AUTH_SYMMETRIC);

  instance->key_id = key_id;

  if (!KEY_KeyKnown(key_id))
    LOG(LOGS_WARN, "Key %"PRIu32" is %s", key_id, "missing");
  else if (!KEY_CheckKeyLength(key_id))
    LOG(LOGS_WARN, "Key %"PRIu32" is %s", key_id, "too short");

  return instance;
}

/* ================================================== */

void
NAU_DestroyInstance(NAU_Instance instance)
{
  Free(instance);
}

/* ================================================== */

int
NAU_IsAuthEnabled(NAU_Instance instance)
{
  return instance->mode != AUTH_NONE;
}

/* ================================================== */

int
NAU_GetSuggestedNtpVersion(NAU_Instance instance)
{
  /* If the MAC in NTPv4 packets would be truncated, prefer NTPv3 for
     compatibility with older chronyd servers */
  if (instance->mode == AUTH_SYMMETRIC &&
      KEY_GetAuthLength(instance->key_id) + sizeof (instance->key_id) > NTP_MAX_V4_MAC_LENGTH)
    return 3;

  return NTP_VERSION;
}

/* ================================================== */

int
NAU_PrepareRequestAuth(NAU_Instance instance)
{
  switch (instance->mode) {
    default:
      break;
  }

  return 1;
}

/* ================================================== */

void
NAU_AdjustRequestTimestamp(NAU_Instance instance, struct timespec *ts)
{
  adjust_timestamp(instance->mode, instance->key_id, ts);
}

/* ================================================== */

int
NAU_GenerateRequestAuth(NAU_Instance instance, NTP_Packet *request, NTP_PacketInfo *info)
{
  switch (instance->mode) {
    case AUTH_NONE:
      break;
    case AUTH_SYMMETRIC:
      if (!generate_symmetric_auth(instance->key_id, request, info))
        return 0;
      break;
    default:
      assert(0);
  }

  return 1;
}

/* ================================================== */

int
NAU_CheckRequestAuth(NTP_Packet *request, NTP_PacketInfo *info)
{
  switch (info->auth.mode) {
    case AUTH_NONE:
      break;
    case AUTH_SYMMETRIC:
      if (!check_symmetric_auth(request, info))
        return 0;
      break;
    case AUTH_MSSNTP:
      /* MS-SNTP requests are not authenticated */
      break;
    default:
      return 0;
  }

  return 1;
}

/* ================================================== */

void
NAU_AdjustResponseTimestamp(NTP_Packet *request, NTP_PacketInfo *info, struct timespec *ts)
{
  adjust_timestamp(info->auth.mode, info->auth.mac.key_id, ts);
}

/* ================================================== */

int
NAU_GenerateResponseAuth(NTP_Packet *request, NTP_PacketInfo *request_info,
                         NTP_Packet *response, NTP_PacketInfo *response_info,
                         NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr)
{
  switch (request_info->auth.mode) {
    case AUTH_NONE:
      break;
    case AUTH_SYMMETRIC:
      if (!generate_symmetric_auth(request_info->auth.mac.key_id, response, response_info))
        return 0;
      break;
    case AUTH_MSSNTP:
      /* Sign the packet asynchronously by ntp_signd */
      if (!NSD_SignAndSendPacket(request_info->auth.mac.key_id, response, response_info,
                                 remote_addr, local_addr))
        return 0;
      /* Don't send the original packet */
      return 0;
    default:
      DEBUG_LOG("Could not authenticate response auth_mode=%d", (int)request_info->auth.mode);
      return 0;
  }

  return 1;
}

/* ================================================== */

int
NAU_CheckResponseAuth(NAU_Instance instance, NTP_Packet *response, NTP_PacketInfo *info)
{
  /* If we don't expect the packet to be authenticated, ignore any
     authentication data in the packet */
  if (instance->mode == AUTH_NONE)
    return 1;

  /* The authentication must match the expected mode */
  if (info->auth.mode != instance->mode)
    return 0;

  switch (info->auth.mode) {
    case AUTH_NONE:
      break;
    case AUTH_SYMMETRIC:
      /* Check if it is authenticated with the specified key */
      if (info->auth.mac.key_id != instance->key_id)
        return 0;
      /* and that the MAC is valid */
      if (!check_symmetric_auth(response, info))
        return 0;
      break;
    default:
      return 0;
  }

  return 1;
}
