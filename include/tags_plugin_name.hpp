/*
 *  Copyright © 2017-2022 Wellington Wallace
 *
 *  This file is part of EasyEffects.
 *
 *  EasyEffects is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  EasyEffects is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with EasyEffects.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <glib/gi18n.h>
#include <array>
#include <iostream>
#include <map>

namespace tags::plugin_name {

inline constexpr auto autogain = "autogain";

inline constexpr auto bass_enhancer = "bass_enhancer";

inline constexpr auto bass_loudness = "bass_loudness";

inline constexpr auto compressor = "compressor";

inline constexpr auto convolver = "convolver";

inline constexpr auto crossfeed = "crossfeed";

inline constexpr auto crystalizer = "crystalizer";

inline constexpr auto deesser = "deesser";

inline constexpr auto delay = "delay";

inline constexpr auto echo_canceller = "echo_canceller";

inline constexpr auto equalizer = "equalizer";

inline constexpr auto exciter = "exciter";

inline constexpr auto filter = "filter";

inline constexpr auto gate = "gate";

inline constexpr auto limiter = "limiter";

inline constexpr auto loudness = "loudness";

inline constexpr auto maximizer = "maximizer";

inline constexpr auto multiband_compressor = "multiband_compressor";

inline constexpr auto multiband_gate = "multiband_gate";

inline constexpr auto pitch = "pitch";

inline constexpr auto reverb = "reverb";

inline constexpr auto rnnoise = "rnnoise";

inline constexpr auto stereo_tools = "stereo_tools";

inline constexpr auto list = std::to_array(
    {autogain,  bass_enhancer,        bass_loudness,  compressor, convolver, crossfeed, crystalizer, deesser,
     delay,     echo_canceller,       equalizer,      exciter,    filter,    gate,      limiter,     loudness,
     maximizer, multiband_compressor, multiband_gate, pitch,      reverb,    rnnoise,   stereo_tools});

auto get_translated() -> std::map<std::string, std::string>;

}  // namespace tags::plugin_name