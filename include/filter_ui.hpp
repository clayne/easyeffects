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

#pragma once

#include <adwaita.h>
#include <glib-object.h>
#include <glibconfig.h>
#include <gtk/gtkbox.h>
#include <memory>
#include <string>
#include "filter.hpp"

namespace ui::filter_box {

G_BEGIN_DECLS

#define EE_TYPE_FILTER_BOX (filter_box_get_type())

G_DECLARE_FINAL_TYPE(FilterBox, filter_box, EE, FILTER_BOX, GtkBox)

G_END_DECLS

auto create() -> FilterBox*;

void setup(FilterBox* self, std::shared_ptr<Filter> filter, const std::string& schema_path);

}  // namespace ui::filter_box
