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

#include "application.hpp"
#include <adwaita.h>
#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <spa/param/param.h>
#include <spa/utils/defs.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ostream>
#include <string>
#include <thread>
#include "application_ui.hpp"
#include "config.h"
#include "pipe_manager.hpp"
#include "pipe_objects.hpp"
#include "preferences_window.hpp"
#include "preset_type.hpp"
#include "presets_manager.hpp"
#include "stream_input_effects.hpp"
#include "stream_output_effects.hpp"
#include "tags_app.hpp"
#include "tags_pipewire.hpp"
#include "tags_resources.hpp"
#include "tags_schema.hpp"
#include "util.hpp"

namespace app {

using namespace std::string_literals;

// NOLINTNEXTLINE
G_DEFINE_TYPE(Application, application, ADW_TYPE_APPLICATION)

void hide_all_windows(GApplication* app) {
  auto* list = gtk_application_get_windows(GTK_APPLICATION(app));

  while (list != nullptr) {
    auto* window = list->data;
    auto* next = list->next;

    gtk_window_destroy(GTK_WINDOW(window));

    list = next;
  }
}

void update_bypass_state(Application* self) {
  const auto state = g_settings_get_boolean(self->settings, "bypass");

  self->soe->set_bypass(state != 0);
  self->sie->set_bypass(state != 0);

  util::info(((state) != 0 ? "enabling" : "disabling") + " global bypass"s);
}

void on_startup(GApplication* gapp) {
  G_APPLICATION_CLASS(application_parent_class)->startup(gapp);

  auto* self = EE_APP(gapp);

  self->data = new Data();

  self->sie_settings = g_settings_new(tags::schema::id_input);
  self->soe_settings = g_settings_new(tags::schema::id_output);

  self->pm = new PipeManager();
  self->soe = new StreamOutputEffects(self->pm);
  self->sie = new StreamInputEffects(self->pm);

  if (self->settings == nullptr) {
    self->settings = g_settings_new(tags::app::id);
  }

  if (self->presets_manager == nullptr) {
    self->presets_manager = new PresetsManager();
  }

  PipeManager::exclude_monitor_stream = g_settings_get_boolean(self->settings, "exclude-monitor-streams") != 0;

  self->data->connections.push_back(self->pm->new_default_sink_name.connect([=](const std::string name) {
    util::debug("new default output device: " + name);

    if (g_settings_get_boolean(self->soe_settings, "use-default-output-device") != 0) {
      g_settings_set_string(self->soe_settings, "output-device", name.c_str());
    }
  }));

  self->data->connections.push_back(self->pm->new_default_source_name.connect([=](const std::string name) {
    util::debug("new default input device: " + name);

    if (g_settings_get_boolean(self->sie_settings, "use-default-input-device") != 0) {
      g_settings_set_string(self->sie_settings, "input-device", name.c_str());
    }
  }));

  self->data->connections.push_back(self->pm->device_input_route_changed.connect([=](const DeviceInfo device) {
    if (device.input_route_available == SPA_PARAM_AVAILABILITY_no) {
      return;
    }

    util::debug("input autoloading: device \"" + device.name + "\" has changed its input route to \"" +
                device.input_route_name + "\"");

    const auto name = util::gsettings_get_string(self->sie_settings, "input-device");

    for (const auto& [serial, node] : self->pm->node_map) {
      if (node.media_class == tags::pipewire::media_class::source && node.device_id == device.id && node.name == name) {
        util::debug("input autoloading: target node \"" + name + "\" matches the input device name");

        self->presets_manager->autoload(PresetType::input, node.name, device.input_route_name);

        return;
      } else {
        util::debug("input autoloading: skip \"" + node.name + "\" candidate since it does not match \"" + name +
                    "\" input device");
      }
    }

    util::debug("input autoloading: no target nodes match the input device name \"" + name + "\"");
  }));

  self->data->connections.push_back(self->pm->device_output_route_changed.connect([=](const DeviceInfo device) {
    if (device.output_route_available == SPA_PARAM_AVAILABILITY_no) {
      return;
    }

    util::debug("output autoloading: device \"" + device.name + "\" has changed its output route to \"" +
                device.output_route_name + "\"");

    const auto name = util::gsettings_get_string(self->soe_settings, "output-device");

    for (const auto& [serial, node] : self->pm->node_map) {
      if (node.media_class == tags::pipewire::media_class::sink && node.device_id == device.id && node.name == name) {
        util::debug("output autoloading: target node \"" + name + "\" matches the output device name");

        self->presets_manager->autoload(PresetType::output, node.name, device.output_route_name);

        return;
      } else {
        util::debug("output autoloading: skip \"" + node.name + "\" candidate since it does not match \"" + name +
                    "\" output device");
      }
    }

    util::debug("output autoloading: no target nodes match the output device name \"" + name + "\"");
  }));

  self->data->gconnections_soe.push_back(g_signal_connect(
      self->soe_settings, "changed::output-device", G_CALLBACK(+[](GSettings* settings, char* key, gpointer user_data) {
        auto* self = static_cast<Application*>(user_data);

        const auto name = util::gsettings_get_string(settings, key);

        if (name.empty()) {
          return;
        }

        for (const auto& device : self->pm->list_devices) {
          if (util::str_contains(name, device.bus_path) || util::str_contains(name, device.bus_id)) {
            self->presets_manager->autoload(PresetType::output, name, device.output_route_name);

            return;
          }
        }
      }),
      self));

  self->data->gconnections_sie.push_back(g_signal_connect(
      self->sie_settings, "changed::input-device", G_CALLBACK(+[](GSettings* settings, char* key, gpointer user_data) {
        auto* self = static_cast<Application*>(user_data);

        const auto name = util::gsettings_get_string(settings, key);

        if (name.empty()) {
          return;
        }

        for (const auto& device : self->pm->list_devices) {
          if (util::str_contains(name, device.bus_path) || util::str_contains(name, device.bus_id)) {
            self->presets_manager->autoload(PresetType::input, name, device.input_route_name);

            return;
          }
        }
      }),
      self));

  self->data->gconnections.push_back(g_signal_connect(
      self->settings, "changed::bypass", G_CALLBACK(+[](GSettings* settings, char* key, gpointer user_data) {
        auto* self = static_cast<Application*>(user_data);

        update_bypass_state(self);
      }),
      self));

  self->data->gconnections.push_back(
      g_signal_connect(self->settings, "changed::exclude-monitor-streams",
                       G_CALLBACK(+[](GSettings* settings, char* key, gpointer user_data) {
                         PipeManager::exclude_monitor_stream = g_settings_get_boolean(settings, key) != 0;
                       }),
                       self));

  update_bypass_state(self);

  if ((g_application_get_flags(gapp) & G_APPLICATION_IS_SERVICE) != 0) {
    g_application_hold(gapp);
  }

  // Debug check PipeWire minimum version
  const auto comparison_result = util::compare_versions(self->pm->version, tags::app::minimum_pw_version);

  switch (comparison_result) {
    case 1:
    case 0:
      // Supported version. Nothing to show...
      break;

    case -1:
      util::warning("PipeWire version " + self->pm->version + " is lower than " + tags::app::minimum_pw_version +
                    " minimum supported.");
      break;

    default:
      util::debug("Cannot check the current PipeWire version against the minimum supported.");
      break;
  }
}

void application_class_init(ApplicationClass* klass) {
  auto* application_class = G_APPLICATION_CLASS(klass);

  application_class->handle_local_options = [](GApplication* gapp, GVariantDict* options) {
    if (options == nullptr) {
      return -1;
    }

    auto* self = EE_APP(gapp);

    if (self->settings == nullptr) {
      self->settings = g_settings_new(tags::app::id);
    }

    if (self->presets_manager == nullptr) {
      self->presets_manager = new PresetsManager();
    }

    if (g_variant_dict_contains(options, "version") != 0) {
      std::cout << "easyeffects version: " << std::string(VERSION) << '\n';

      return EXIT_SUCCESS;
    }

    if (g_variant_dict_contains(options, "presets") != 0) {
      std::string list;

      for (const auto& name : self->presets_manager->get_local_presets_name(PresetType::output)) {
        list += name + ",";
      }

      std::cout << _("Output Presets") + ": "s + list << '\n';

      list = "";

      for (const auto& name : self->presets_manager->get_local_presets_name(PresetType::input)) {
        list += name + ",";
      }

      std::cout << _("Input Presets") + ": "s + list << '\n';

      return EXIT_SUCCESS;
    }

    if (g_variant_dict_contains(options, "active-presets") != 0) {
      const auto& output_preset = self->presets_manager->get_loaded_preset(PresetType::output);
      const auto& input_preset = self->presets_manager->get_loaded_preset(PresetType::input);

      std::cout << _("Output Preset") + ": "s + output_preset << '\n';
      std::cout << _("Input Preset") + ": "s + input_preset << '\n';

      return EXIT_SUCCESS;
    }

    if (g_variant_dict_contains(options, "active-preset") != 0) {
      const char* value = nullptr;

      if (g_variant_dict_lookup(options, "active-preset", "&s", &value) != 0) {
        if (strcmp(value, "input") != 0 && strcmp(value, "output") != 0) {
          util::error("active-preset must have a value of input or output");

          return EXIT_FAILURE;
        } else {
          const PresetType& type = (strcmp(value, "input") == 0) ? PresetType::input : PresetType::output;

          const auto& preset = self->presets_manager->get_loaded_preset(type);

          std::cout << preset << '\n';

          return EXIT_SUCCESS;
        }
      }
    }

    if (g_variant_dict_contains(options, "bypass") != 0) {
      if (int bypass_arg = 2; g_variant_dict_lookup(options, "bypass", "i", &bypass_arg)) {
        if (bypass_arg == 3) {
          std::cout << g_settings_get_boolean(self->settings, "bypass") << '\n';

          return EXIT_SUCCESS;
        }
      }
    }

    return -1;
  };

  application_class->startup = on_startup;

  application_class->command_line = [](GApplication* gapp, GApplicationCommandLine* cmdline) {
    auto* self = EE_APP(gapp);
    GVariantDict* options = g_application_command_line_get_options_dict(cmdline);

    if (g_variant_dict_contains(options, "quit") != 0) {
      hide_all_windows(gapp);

      g_application_quit(G_APPLICATION(gapp));

      return EXIT_SUCCESS;
    }

    if (g_variant_dict_contains(options, "load-preset") != 0) {
      const char* name = nullptr;

      if (g_variant_dict_lookup(options, "load-preset", "&s", &name) != 0) {
        if (self->presets_manager->preset_file_exists(PresetType::input, name)) {
          self->presets_manager->load_local_preset_file(PresetType::input, name);

          return EXIT_SUCCESS;
        }

        if (self->presets_manager->preset_file_exists(PresetType::output, name)) {
          self->presets_manager->load_local_preset_file(PresetType::output, name);

          return EXIT_SUCCESS;
        }
      }
    }

    if (g_variant_dict_contains(options, "reset") != 0) {
      util::reset_all_keys_except(self->settings);

      self->soe->reset_settings();
      self->sie->reset_settings();

      util::info("All settings were reset");

      return EXIT_SUCCESS;
    }

    if (g_variant_dict_contains(options, "hide-window") != 0) {
      hide_all_windows(gapp);

      util::info("Hiding the window...");

      return EXIT_SUCCESS;
    }

    if (g_variant_dict_contains(options, "bypass") != 0) {
      if (int bypass_arg = 2; g_variant_dict_lookup(options, "bypass", "i", &bypass_arg)) {
        if (bypass_arg == 1) {
          g_settings_set_boolean(self->settings, "bypass", 1);
        } else if (bypass_arg == 2) {
          g_settings_set_boolean(self->settings, "bypass", 0);
        }

        return EXIT_SUCCESS;
      }
    }

    g_application_activate(gapp);

    return G_APPLICATION_CLASS(application_parent_class)->command_line(gapp, cmdline);
  };

  application_class->activate = [](GApplication* gapp) {
    if (gtk_application_get_active_window(GTK_APPLICATION(gapp)) == nullptr) {
      G_APPLICATION_CLASS(application_parent_class)->activate(gapp);

      auto* window = ui::application_window::create(gapp);

      gtk_window_present(GTK_WINDOW(window));
    }
  };

  application_class->shutdown = [](GApplication* gapp) {
    G_APPLICATION_CLASS(application_parent_class)->shutdown(gapp);

    auto* self = EE_APP(gapp);

    for (auto& c : self->data->connections) {
      c.disconnect();
    }

    for (auto& handler_id : self->data->gconnections) {
      g_signal_handler_disconnect(self->settings, handler_id);
    }

    for (auto& handler_id : self->data->gconnections_sie) {
      g_signal_handler_disconnect(self->sie_settings, handler_id);
    }

    for (auto& handler_id : self->data->gconnections_soe) {
      g_signal_handler_disconnect(self->soe_settings, handler_id);
    }

    self->data->connections.clear();
    self->data->gconnections_sie.clear();
    self->data->gconnections_soe.clear();

    g_object_unref(self->settings);
    g_object_unref(self->sie_settings);
    g_object_unref(self->soe_settings);

    PipeManager::exiting = true;

    delete self->data;
    delete self->presets_manager;
    delete self->sie;
    delete self->soe;
    delete self->pm;

    self->data = nullptr;
    self->presets_manager = nullptr;
    self->sie = nullptr;
    self->soe = nullptr;
    self->pm = nullptr;

    util::debug("Shutting down...");
  };
}

void application_init(Application* self) {
  std::array<GActionEntry, 8> entries{};

  entries[0] = {"quit",
                [](GSimpleAction* action, GVariant* parameter, gpointer app) {
                  util::debug("The user pressed <Ctrl>Q or executed a similar action. Our process will exit.");

                  g_application_quit(G_APPLICATION(app));
                },
                nullptr, nullptr, nullptr};

  entries[1] = {"help",
                [](GSimpleAction* action, GVariant* parameter, gpointer gapp) {
                  /*
                    It isn't secure to call std::system but it is the only way to make our help to be shown for KDE
                    users.
                  */

                  if (g_strcmp0(std::getenv("XDG_SESSION_DESKTOP"), "KDE") == 0 ||
                      g_strcmp0(std::getenv("XDG_CURRENT_DESKTOP"), "KDE") == 0) {
                    std::thread t([]() { return std::system("yelp help:easyeffects"); });

                    t.detach();

                    return;
                  }

                  auto* parent = gtk_application_get_active_window(GTK_APPLICATION(gapp));

                  auto* launcher = gtk_uri_launcher_new("help:easyeffects");

                  gtk_uri_launcher_launch(launcher, parent, nullptr, nullptr, nullptr);

                  g_object_unref(launcher);
                },
                nullptr, nullptr, nullptr};

  entries[2] = {
      "about",
      [](GSimpleAction* action, GVariant* parameter, gpointer gapp) {
        std::array<const char*, 4> developers = {"Wellington Wallace <wellingtonwallace@gmail.com>",
                                                 "Giusy Digital <kurmikon@libero.it>", "Vincent Chernin", nullptr};

        std::array<const char*, 3> documenters = {"Wellington Wallace <wellingtonwallace@gmail.com>",
                                                  "Giusy Digital <kurmikon@libero.it>", nullptr};

        adw_show_about_dialog(GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(gapp))), "application-name",
                              APP_NAME, "version", VERSION, "developer-name", "Wellington Wallace", "developers",
                              developers.data(), "application-icon",
                              IS_DEVEL_BUILD ? std::string(tags::app::id).append(".Devel").c_str() : tags::app::id,
                              "copyright", "Copyright © 2017–2022 Easy Effects Contributors", "license-type",
                              GTK_LICENSE_GPL_3_0, "website", "https://github.com/wwmm/easyeffects", "debug-info",
                              std::string("Commit: ").append(COMMIT_DESC).c_str(), "translator-credits",
                              _("Weblate https://hosted.weblate.org/projects/easyeffects/"), "documenters",
                              documenters.data(), "issue-url", "https://github.com/wwmm/easyeffects/issues", nullptr);
      },
      nullptr, nullptr, nullptr};

  entries[3] = {"fullscreen",
                [](GSimpleAction* action, GVariant* parameter, gpointer gapp) {
                  auto* self = EE_APP(gapp);

                  auto state = g_settings_get_boolean(self->settings, "window-fullscreen") != 0;

                  if (state) {
                    gtk_window_unfullscreen(GTK_WINDOW(gtk_application_get_active_window(GTK_APPLICATION(gapp))));

                    g_settings_set_boolean(self->settings, "window-fullscreen", 0);
                  } else {
                    gtk_window_fullscreen(GTK_WINDOW(gtk_application_get_active_window(GTK_APPLICATION(gapp))));

                    g_settings_set_boolean(self->settings, "window-fullscreen", 1);
                  }
                },
                nullptr, nullptr, nullptr};

  entries[4] = {"preferences",
                [](GSimpleAction* action, GVariant* parameter, gpointer gapp) {
                  auto* preferences = ui::preferences::window::create();

                  adw_dialog_present(ADW_DIALOG(preferences),
                                     GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(gapp))));
                },
                nullptr, nullptr, nullptr};

  entries[5] = {"reset",
                [](GSimpleAction* action, GVariant* parameter, gpointer gapp) {
                  auto* self = EE_APP(gapp);

                  util::reset_all_keys_except(self->settings);

                  self->soe->reset_settings();
                  self->sie->reset_settings();
                },
                nullptr, nullptr, nullptr};

  entries[6] = {"shortcuts",
                [](GSimpleAction* action, GVariant* parameter, gpointer gapp) {
                  auto* builder = gtk_builder_new_from_resource(tags::resources::shortcuts_ui);

                  auto* window = GTK_SHORTCUTS_WINDOW(gtk_builder_get_object(builder, "window"));

                  gtk_window_set_transient_for(GTK_WINDOW(window),
                                               GTK_WINDOW(gtk_application_get_active_window(GTK_APPLICATION(gapp))));

                  gtk_window_present(GTK_WINDOW(window));

                  g_object_unref(builder);
                },
                nullptr, nullptr, nullptr};

  entries[7] = {"hide_windows",
                [](GSimpleAction* action, GVariant* parameter, gpointer app) {
                  util::debug("The user pressed <Ctrl>W or executed a similar action. Hiding our window.");

                  hide_all_windows(G_APPLICATION(app));
                },
                nullptr, nullptr, nullptr};

  g_action_map_add_action_entries(G_ACTION_MAP(self), entries.data(), entries.size(), self);

  std::array<const char*, 2> quit_accels = {"<Ctrl>Q", nullptr};
  std::array<const char*, 2> hide_windows_accels = {"<Ctrl>W", nullptr};

  std::array<const char*, 2> help_accels = {"F1", nullptr};
  std::array<const char*, 2> fullscreen_accels = {"F11", nullptr};

  gtk_application_set_accels_for_action(GTK_APPLICATION(self), "app.quit", quit_accels.data());
  gtk_application_set_accels_for_action(GTK_APPLICATION(self), "app.hide_windows", hide_windows_accels.data());
  gtk_application_set_accels_for_action(GTK_APPLICATION(self), "app.help", help_accels.data());
  gtk_application_set_accels_for_action(GTK_APPLICATION(self), "app.fullscreen", fullscreen_accels.data());
}

auto application_new() -> GApplication* {
  g_set_application_name("Easy Effects");

  auto* app = g_object_new(EE_TYPE_APPLICATION, "application-id",
                           IS_DEVEL_BUILD ? std::string(tags::app::id).append(".Devel").c_str() : tags::app::id,
                           "flags", G_APPLICATION_HANDLES_COMMAND_LINE, nullptr);

  g_application_add_main_option(G_APPLICATION(app), "quit", 'q', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                _("Quit Easy Effects. Useful when running in service mode."), nullptr);

  g_application_add_main_option(G_APPLICATION(app), "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                _("Print the easyeffects version"), nullptr);

  g_application_add_main_option(G_APPLICATION(app), "reset", 'r', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                _("Reset Easy Effects."), nullptr);

  g_application_add_main_option(G_APPLICATION(app), "hide-window", 'w', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                _("Hide the Window."), nullptr);

  g_application_add_main_option(G_APPLICATION(app), "bypass", 'b', G_OPTION_FLAG_NONE, G_OPTION_ARG_INT,
                                _("Global bypass. 1 to enable, 2 to disable and 3 to get status"), nullptr);

  g_application_add_main_option(G_APPLICATION(app), "presets", 'p', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                _("Show available presets."), nullptr);

  g_application_add_main_option(G_APPLICATION(app), "load-preset", 'l', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                _("Load a preset. Example: easyeffects -l music"), nullptr);

  g_application_add_main_option(G_APPLICATION(app), "active-presets", 'a', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                _("Show the active presets."), nullptr);

  g_application_add_main_option(G_APPLICATION(app), "active-preset", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                _("Show the loaded preset of a specific category. Takes 'input' or 'output' as a "
                                  "value. Example: easyeffects -s input"),
                                nullptr);

  return G_APPLICATION(app);
}

}  // namespace app
