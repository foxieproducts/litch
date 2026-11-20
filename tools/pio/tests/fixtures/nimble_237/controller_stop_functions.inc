/*
 * NimBLE-Arduino 2.3.7 function excerpts, unmodified.
 * NimBLEDevice.cpp: Copyright 2020-2025 Ryan Powell <ryan@nable-embedded.io>
 * and esp-nimble-cpp, NimBLE-Arduino contributors.
 * nimble_port.c: Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the upstream NOTICE file.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Native tests supply SDK leaves and declarations, not replacement algorithms.
 */
esp_err_t
nimble_port_deinit(void)
{
    esp_err_t ret;

    ret = esp_nimble_deinit();
    if(ret != ESP_OK) {
        ESP_LOGE(NIMBLE_PORT_LOG_TAG, "nimble host deinit failed\n");
        return ret;
    }

#if CONFIG_BT_CONTROLLER_ENABLED
    ret = esp_bt_controller_disable();
    if(ret != ESP_OK) {
        ESP_LOGE(NIMBLE_PORT_LOG_TAG, "controller disable failed\n");
        return ret;
    }

    ret = esp_bt_controller_deinit();
    if(ret != ESP_OK) {
        ESP_LOGE(NIMBLE_PORT_LOG_TAG, "controller deinit failed\n");
        return ret;
    }
#endif

#if CONFIG_BT_LE_CONTROLLER_NPL_OS_PORTING_SUPPORT
    na_hci_transport_deinit();
#endif

#if (BT_HCI_LOG_INCLUDED == TRUE)
    //bt_hci_log_deinit();
#endif // (BT_HCI_LOG_INCLUDED == TRUE)

    return ESP_OK;
}

bool NimBLEDevice::deinit(bool clearAll) {
    int rc = 0;
    if (m_initialized) {
        rc = nimble_port_stop();
        if (rc == 0) {
            nimble_port_deinit();
# ifdef CONFIG_NIMBLE_CPP_IDF
#  if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
            rc = esp_nimble_hci_and_controller_deinit();
            if (rc != ESP_OK) {
                NIMBLE_LOGE(LOG_TAG, "esp_nimble_hci_and_controller_deinit() failed with error: %d", rc);
            }
#  endif
# endif
            m_initialized = false;
            m_synced      = false;
        }
    }

    if (clearAll) {
# if CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
        if (NimBLEDevice::m_pServer != nullptr) {
            delete NimBLEDevice::m_pServer;
            NimBLEDevice::m_pServer = nullptr;
        }
#  if CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM
        if (NimBLEDevice::m_pL2CAPServer != nullptr) {
            delete NimBLEDevice::m_pL2CAPServer;
            NimBLEDevice::m_pL2CAPServer = nullptr;
        }
#  endif
# endif

# if CONFIG_BT_NIMBLE_ROLE_BROADCASTER
        if (NimBLEDevice::m_bleAdvertising != nullptr) {
            delete NimBLEDevice::m_bleAdvertising;
            NimBLEDevice::m_bleAdvertising = nullptr;
        }
# endif

# if CONFIG_BT_NIMBLE_ROLE_OBSERVER
        if (NimBLEDevice::m_pScan != nullptr) {
            delete NimBLEDevice::m_pScan;
            NimBLEDevice::m_pScan = nullptr;
        }
# endif

# if CONFIG_BT_NIMBLE_ROLE_CENTRAL
        for (auto clt : m_pClients) {
            deleteClient(clt);
        }
# endif
    }

    return rc == 0;
}
