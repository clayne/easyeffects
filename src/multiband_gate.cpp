/*
 *  Copyright © 2017-2025 Wellington Wallace
 *
 *  This file is part of Easy Effects.
 *
 *  Easy Effects is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Easy Effects is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Easy Effects. If not, see <https://www.gnu.org/licenses/>.
 */

#include "multiband_gate.hpp"
#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <sys/types.h>
#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include "lv2_wrapper.hpp"
#include "pipe_manager.hpp"
#include "pipe_objects.hpp"
#include "plugin_base.hpp"
#include "tags_plugin_name.hpp"
#include "util.hpp"

MultibandGate::MultibandGate(const std::string& tag,
                             const std::string& schema,
                             const std::string& schema_path,
                             PipeManager* pipe_manager,
                             PipelineType pipe_type)
    : PluginBase(tag,
                 tags::plugin_name::multiband_gate,
                 tags::plugin_package::lsp,
                 schema,
                 schema_path,
                 pipe_manager,
                 pipe_type,
                 true) {
  lv2_wrapper = std::make_unique<lv2::Lv2Wrapper>("http://lsp-plug.in/plugins/lv2/sc_mb_gate_stereo");

  package_installed = lv2_wrapper->found_plugin;

  if (!package_installed) {
    util::debug(log_tag + "http://lsp-plug.in/plugins/lv2/sc_mb_gate_stereo is not installed");
  }

  gconnections.push_back(g_signal_connect(settings, "changed::sidechain-input-device",
                                          G_CALLBACK(+[](GSettings* settings, char* key, gpointer user_data) {
                                            auto* self = static_cast<MultibandGate*>(user_data);

                                            self->update_sidechain_links(key);
                                          }),
                                          this));

  lv2_wrapper->bind_key_enum<"mode", "gate-mode">(settings);

  lv2_wrapper->bind_key_enum<"envb", "envelope-boost">(settings);

  lv2_wrapper->bind_key_bool<"ssplit", "stereo-split">(settings);

  // The following controls can assume -inf
  lv2_wrapper->bind_key_double_db<"g_dry", "dry", false>(settings);

  lv2_wrapper->bind_key_double_db<"g_wet", "wet", false>(settings);

  bind_bands(std::make_index_sequence<n_bands>());

  setup_input_output_gain();
}

MultibandGate::~MultibandGate() {
  if (connected_to_pw) {
    disconnect_from_pw();
  }

  util::debug(log_tag + name + " destroyed");
}

void MultibandGate::setup() {
  if (!lv2_wrapper->found_plugin) {
    return;
  }

  lv2_wrapper->set_n_samples(n_samples);

  if (lv2_wrapper->get_rate() != rate) {
    lv2_wrapper->create_instance(rate);
  }
}

void MultibandGate::process(std::span<float>& left_in,
                            std::span<float>& right_in,
                            std::span<float>& left_out,
                            std::span<float>& right_out,
                            std::span<float>& probe_left,
                            std::span<float>& probe_right) {
  if (!lv2_wrapper->found_plugin || !lv2_wrapper->has_instance() || bypass) {
    std::copy(left_in.begin(), left_in.end(), left_out.begin());
    std::copy(right_in.begin(), right_in.end(), right_out.begin());

    return;
  }

  if (input_gain != 1.0F) {
    apply_gain(left_in, right_in, input_gain);
  }

  lv2_wrapper->connect_data_ports(left_in, right_in, left_out, right_out, probe_left, probe_right);
  lv2_wrapper->run();

  if (output_gain != 1.0F) {
    apply_gain(left_out, right_out, output_gain);
  }

  /*
   This plugin gives the latency in number of samples
 */

  const auto lv = static_cast<uint>(lv2_wrapper->get_control_port_value("out_latency"));

  if (latency_n_frames != lv) {
    latency_n_frames = lv;

    latency_value = static_cast<float>(latency_n_frames) / static_cast<float>(rate);

    util::debug(log_tag + name + " latency: " + util::to_string(latency_value, "") + " s");

    util::idle_add([this]() {
      if (!post_messages || latency.empty()) {
        return;
      }

      latency.emit();
    });

    update_filter_params();
  }

  if (post_messages) {
    get_peaks(left_in, right_in, left_out, right_out);

    if (send_notifications) {
      for (uint n = 0U; n < n_bands; n++) {
        const auto nstr = util::to_string(n);

        frequency_range_end_port_array.at(n) = lv2_wrapper->get_control_port_value("fre_" + nstr);

        envelope_port_array.at(n) = 0.5F * (lv2_wrapper->get_control_port_value("elm_" + nstr + "l") +
                                            lv2_wrapper->get_control_port_value("elm_" + nstr + "r"));

        curve_port_array.at(n) = 0.5F * (lv2_wrapper->get_control_port_value("clm_" + nstr + "l") +
                                         lv2_wrapper->get_control_port_value("clm_" + nstr + "r"));

        reduction_port_array.at(n) = 0.5F * (lv2_wrapper->get_control_port_value("rlm_" + nstr + "l") +
                                             lv2_wrapper->get_control_port_value("rlm_" + nstr + "r"));
      }

      frequency_range.emit(frequency_range_end_port_array);
      envelope.emit(envelope_port_array);
      curve.emit(curve_port_array);
      reduction.emit(reduction_port_array);

      notify();
    }
  }
}

void MultibandGate::update_sidechain_links(const std::string& key) {
  auto external_sidechain_enabled = false;

  for (uint n = 0U; !external_sidechain_enabled && n < n_bands; n++) {
    const auto nstr = util::to_string(n);

    external_sidechain_enabled = g_settings_get_boolean(settings, ("external-sidechain" + nstr).c_str()) != 0;
  }

  if (!external_sidechain_enabled) {
    pm->destroy_links(list_proxies);

    list_proxies.clear();

    return;
  }

  const auto device_name = util::gsettings_get_string(settings, "sidechain-input-device");

  NodeInfo input_device = pm->ee_source_node;

  for (const auto& [serial, node] : pm->node_map) {
    if (node.name == device_name) {
      input_device = node;

      break;
    }
  }

  pm->destroy_links(list_proxies);

  list_proxies.clear();

  for (const auto& link : pm->link_nodes(input_device.id, get_node_id(), true)) {
    list_proxies.push_back(link);
  }
}

void MultibandGate::update_probe_links() {
  update_sidechain_links("");
}

auto MultibandGate::get_latency_seconds() -> float {
  return 0.0F;
}
