/******************************************************************************
 *
 *  Copyright 2003-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

// Exact functions from Android Bluetooth e6130675c04752947ac4779c178ce70eb959a97f.
// system/stack/smp/smp_act.cc, blob 7fb626d97fc8ebbc85e82f2ffa373c5a4ac3b99d.
// Full source SHA-256: b9f373967d7e6837c7baff9d4ace59b1cb4a9ce314faa3e5614733634a4da47e.
// Included by the host consent-stage test with controlled crypto/RF/profile leaves.

void smp_proc_sec_req(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  if (smp_command_has_invalid_length(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  tBTM_LE_AUTH_REQ auth_req = *(tBTM_LE_AUTH_REQ*)p_data->p_data;
  tBTM_BLE_SEC_REQ_ACT sec_req_act;

  log::verbose("auth_req=0x{:x}", auth_req);

  p_cb->cb_evt = SMP_EVT_NONE;

  btm_ble_link_sec_check(p_cb->pairing_bda, auth_req, &sec_req_act);

  log::verbose("sec_req_act={}", sec_req_act);

  switch (sec_req_act) {
    case BTM_BLE_SEC_REQ_ACT_ENCRYPT:
      log::verbose("BTM_BLE_SEC_REQ_ACT_ENCRYPT");
      smp_sm_event(p_cb, SMP_ENC_REQ_EVT, NULL);
      break;

    case BTM_BLE_SEC_REQ_ACT_PAIR:
      p_cb->sc_only_mode_locally_required =
          (p_cb->init_security_mode == BTM_SEC_MODE_SC) ? true : false;

      /* respond to non SC pairing request as failure in SC only mode */
      if (p_cb->sc_only_mode_locally_required &&
          (auth_req & SMP_SC_SUPPORT_BIT) == 0) {
        tSMP_INT_DATA smp_int_data;
        smp_int_data.status = SMP_PAIR_AUTH_FAIL;
        smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
      } else {
        /* initialize local i/r key to be default keys */
        p_cb->peer_auth_req = auth_req;
        p_cb->local_r_key = p_cb->local_i_key = SMP_SEC_DEFAULT_KEY;
        p_cb->cb_evt = SMP_SEC_REQUEST_EVT;
      }
      break;

    case BTM_BLE_SEC_REQ_ACT_DISCARD:
      p_cb->discard_sec_req = true;
      break;

    default:
      /* do nothing */
      break;
  }
}

void smp_process_peer_nonce(tSMP_CB* p_cb, tSMP_INT_DATA* /* p_data */) {
  log::verbose("addr:{}, selected_association_model:{}", p_cb->pairing_bda,
               p_cb->selected_association_model);

  // PTS Testing failure modes
  if (p_cb->cert_failure == SMP_CONFIRM_VALUE_ERR) {
    log::error("failure case={}", p_cb->cert_failure);
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_CONFIRM_VALUE_ERR;
    p_cb->failure = SMP_CONFIRM_VALUE_ERR;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }
  // PTS Testing failure modes (for LT)
  if ((p_cb->cert_failure == SMP_NUMERIC_COMPAR_FAIL) &&
      (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_JUSTWORKS) &&
      (p_cb->role == HCI_ROLE_PERIPHERAL)) {
    log::error("failure case={}", p_cb->cert_failure);
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_NUMERIC_COMPAR_FAIL;
    p_cb->failure = SMP_NUMERIC_COMPAR_FAIL;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  switch (p_cb->selected_association_model) {
    case SMP_MODEL_SEC_CONN_JUSTWORKS:
    case SMP_MODEL_SEC_CONN_NUM_COMP:
      /* in these models only central receives commitment */
      if (p_cb->role == HCI_ROLE_CENTRAL) {
        if (!smp_check_commitment(p_cb)) {
          tSMP_INT_DATA smp_int_data;
          smp_int_data.status = SMP_CONFIRM_VALUE_ERR;
          p_cb->failure = SMP_CONFIRM_VALUE_ERR;
          smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
          break;
        }
      } else {
        /* peripheral sends local nonce */
        smp_send_rand(p_cb, NULL);
      }

      if (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_JUSTWORKS) {
        if (!GetInterfaceToProfiles()->config->isAndroidTVDevice() &&
            (p_cb->local_io_capability == SMP_IO_CAP_IO ||
             p_cb->local_io_capability == SMP_IO_CAP_KBDISP)) {
          /* display consent dialog */
          log::verbose("JUST WORKS showing Consent Dialog");
          p_cb->cb_evt = SMP_CONSENT_REQ_EVT;
          smp_set_state(SMP_STATE_WAIT_NONCE);
          smp_sm_event(p_cb, SMP_SC_DSPL_NC_EVT, NULL);
        } else {
          /* go directly to phase 2 */
          smp_sm_event(p_cb, SMP_SC_PHASE1_CMPLT_EVT, NULL);
        }
      } else /* numeric comparison */
      {
        smp_set_state(SMP_STATE_WAIT_NONCE);
        smp_sm_event(p_cb, SMP_SC_CALC_NC_EVT, NULL);
      }
      break;
    case SMP_MODEL_SEC_CONN_PASSKEY_ENT:
    case SMP_MODEL_SEC_CONN_PASSKEY_DISP:
      if (!smp_check_commitment(p_cb) &&
          p_cb->cert_failure != SMP_NUMERIC_COMPAR_FAIL) {
        tSMP_INT_DATA smp_int_data;
        smp_int_data.status = SMP_CONFIRM_VALUE_ERR;
        p_cb->failure = SMP_CONFIRM_VALUE_ERR;
        smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
        break;
      }

      if (p_cb->role == HCI_ROLE_PERIPHERAL) {
        smp_send_rand(p_cb, NULL);
      }

      if (++p_cb->round < 20) {
        smp_set_state(SMP_STATE_SEC_CONN_PHS1_START);
        p_cb->flags &= ~SMP_PAIR_FLAG_HAVE_PEER_COMM;
        smp_start_nonce_generation(p_cb);
        break;
      }

      smp_sm_event(p_cb, SMP_SC_PHASE1_CMPLT_EVT, NULL);
      break;
    case SMP_MODEL_SEC_CONN_OOB:
      if (p_cb->role == HCI_ROLE_PERIPHERAL) {
        smp_send_rand(p_cb, NULL);
      }

      smp_sm_event(p_cb, SMP_SC_PHASE1_CMPLT_EVT, NULL);
      break;
    default:
      log::error("Association Model={} is not used in LE SC",
                 p_cb->selected_association_model);
      break;
  }
}
