// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include "app_hello.h"
#include "gsp_sim_bridge.h"

/* The bridge ABI has no caller context. Keep only its owned opaque handle here. */
static struct {
    app_hello_handle_t controller;
} backend;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    const char *qr = getenv("AI_RECORDER_PROVISION_QR");
    esp_gsp_err_t err = app_hello_create(ui, &backend.controller);
    if (err == ESP_GSP_OK && qr && qr[0] != '\0') {
        err = app_hello_offer_provision(backend.controller, qr);
    }
    return err;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    app_hello_snapshot_t snapshot;
    if (app_hello_read(backend.controller, &snapshot) == ESP_GSP_OK) {
        fprintf(stderr, "recorder_backend: sessions=%u, pending=%u, recording=%d, last_error=%d, provision=%d, deferred=%d\n",
                (unsigned)snapshot.session_count, (unsigned)snapshot.pending_count,
                (int)snapshot.recording, (int)snapshot.last_error,
                (int)app_hello_provision_visible(backend.controller),
                (int)app_hello_provision_deferred(backend.controller));
    }
    const esp_gsp_err_t err = app_hello_destroy(&backend.controller);
    if (err != ESP_GSP_OK) {
        fprintf(stderr, "recorder_backend: cleanup failed (%d)\n", err);
    }
}
