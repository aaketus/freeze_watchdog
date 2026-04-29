/*
freeze_watchdog
Copyright (C) 2026 aake

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "dev"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("freeze-watchdog", "en-US")

MODULE_EXPORT const char* obs_module_description(void)
{
    return "Freeze Watchdog v" PLUGIN_VERSION " - Monitors sources for freezes and resets them.";
}

namespace {

    const uint64_t kPendingScreenshotTimeoutMs = 5000;

    static std::ofstream g_reset_log;
    static std::ofstream g_error_log;

    static void write_log(std::ofstream& stream, const char* fmt, va_list args)
    {
        if (!stream.is_open())
            return;

        char message[2048];
#if defined(_MSC_VER)
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
#else
        vsnprintf(message, sizeof(message), fmt, args);
#endif

        auto now = std::chrono::system_clock::now();
        time_t t = std::chrono::system_clock::to_time_t(now);
        char ts[64] = {};

#if defined(_MSC_VER)
        tm local_tm{};
        localtime_s(&local_tm, &t);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &local_tm);
#else
        tm local_tm{};
        localtime_r(&t, &local_tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &local_tm);
#endif

        stream << "[" << ts << "] " << message << std::endl;
        stream.flush();
    }

    static void write_reset_log(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        write_log(g_reset_log, fmt, args);
        va_end(args);
    }

    static void write_error_log(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        write_log(g_error_log, fmt, args);
        va_end(args);
    }

    struct HiddenSceneItem {
        obs_sceneitem_t* item = nullptr;
    };

    struct SourceState {
        std::vector<uint8_t> signature;
        uint64_t last_check_ms = 0;
        int unchanged_hits = 0;
        int blank_hits = 0;
        int flat_hits = 0;
        int invalid_size_hits = 0;
    };

    struct WatchdogSettings {
        bool enabled = true;
        uint64_t interval_ms = 10000;
        uint64_t hide_duration_ms = 1500;
        uint64_t cooldown_ms = 45000;
        int unchanged_hits_required = 4;
        int unchanged_threshold = 0;
        int blank_hits_required = 2;
        int blank_dark_threshold = 5;
        int blank_bright_threshold = 250;
        int flat_hits_required = 2;
        int flat_range_threshold = 3;
        int invalid_size_hits_required = 3;
        std::vector<std::string> watched_sources;
    };

    static WatchdogSettings default_settings()
    {
        return WatchdogSettings{};
    }

    static uint64_t now_ms()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    static std::string module_config_file_path(const char* file_name)
    {
        char* path = obs_module_config_path(file_name);
        if (!path)
            return {};

        std::string out(path);
        bfree(path);
        return out;
    }

    static obs_data_t* settings_to_obs_data(const WatchdogSettings& s)
    {
        obs_data_t* data = obs_data_create();

        obs_data_set_bool(data, "enabled", s.enabled);
        obs_data_set_int(data, "interval_ms", (long long)s.interval_ms);
        obs_data_set_int(data, "hide_duration_ms", (long long)s.hide_duration_ms);
        obs_data_set_int(data, "cooldown_seconds", (long long)(s.cooldown_ms / 1000ULL));
        obs_data_set_int(data, "unchanged_hits_required", s.unchanged_hits_required);
        obs_data_set_int(data, "unchanged_threshold", s.unchanged_threshold);
        obs_data_set_int(data, "blank_hits_required", s.blank_hits_required);
        obs_data_set_int(data, "blank_dark_threshold", s.blank_dark_threshold);
        obs_data_set_int(data, "blank_bright_threshold", s.blank_bright_threshold);
        obs_data_set_int(data, "flat_hits_required", s.flat_hits_required);
        obs_data_set_int(data, "flat_range_threshold", s.flat_range_threshold);
        obs_data_set_int(data, "invalid_size_hits_required", s.invalid_size_hits_required);

        obs_data_array_t* arr = obs_data_array_create();
        for (const auto& name : s.watched_sources) {
            obs_data_t* item = obs_data_create();
            obs_data_set_string(item, "value", name.c_str());
            obs_data_array_push_back(arr, item);
            obs_data_release(item);
        }
        obs_data_set_array(data, "sources", arr);
        obs_data_array_release(arr);

        return data;
    }

    static void apply_obs_data_to_settings(obs_data_t* data, WatchdogSettings& s)
    {
        if (!data)
            return;

        s.enabled = obs_data_get_bool(data, "enabled");
        s.interval_ms = (uint64_t)obs_data_get_int(data, "interval_ms");
        s.hide_duration_ms = (uint64_t)obs_data_get_int(data, "hide_duration_ms");
        s.cooldown_ms = (uint64_t)obs_data_get_int(data, "cooldown_seconds") * 1000ULL;
        s.unchanged_hits_required = (int)obs_data_get_int(data, "unchanged_hits_required");
        s.unchanged_threshold = (int)obs_data_get_int(data, "unchanged_threshold");
        s.blank_hits_required = (int)obs_data_get_int(data, "blank_hits_required");
        s.blank_dark_threshold = (int)obs_data_get_int(data, "blank_dark_threshold");
        s.blank_bright_threshold = (int)obs_data_get_int(data, "blank_bright_threshold");
        s.flat_hits_required = (int)obs_data_get_int(data, "flat_hits_required");
        s.flat_range_threshold = (int)obs_data_get_int(data, "flat_range_threshold");
        s.invalid_size_hits_required = (int)obs_data_get_int(data, "invalid_size_hits_required");

        s.watched_sources.clear();

        obs_data_array_t* arr = obs_data_get_array(data, "sources");
        if (arr) {
            const size_t count = obs_data_array_count(arr);
            for (size_t i = 0; i < count; ++i) {
                obs_data_t* item = obs_data_array_item(arr, i);
                if (item) {
                    const char* value = obs_data_get_string(item, "value");
                    if (value && *value)
                        s.watched_sources.emplace_back(value);
                    obs_data_release(item);
                }
            }
            obs_data_array_release(arr);
        }

        std::sort(s.watched_sources.begin(), s.watched_sources.end());
        s.watched_sources.erase(std::unique(s.watched_sources.begin(), s.watched_sources.end()),
            s.watched_sources.end());

        if (s.interval_ms < 1000)
            s.interval_ms = 1000;
        if (s.hide_duration_ms < 100)
            s.hide_duration_ms = 100;
        if (s.cooldown_ms < 1000)
            s.cooldown_ms = 1000;
        if (s.blank_bright_threshold < 155)
            s.blank_bright_threshold = 155;
    }

    static WatchdogSettings load_settings()
    {
        WatchdogSettings s = default_settings();
        const std::string path = module_config_file_path("watchdog.json");
        if (path.empty())
            return s;

        obs_data_t* data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
        if (!data)
            return s;

        obs_data_t* defaults = settings_to_obs_data(default_settings());
        obs_data_apply(data, defaults);
        obs_data_release(defaults);

        apply_obs_data_to_settings(data, s);
        obs_data_release(data);
        return s;
    }

    static bool save_settings(const WatchdogSettings& s)
    {
        const std::string path = module_config_file_path("watchdog.json");
        if (path.empty()) {
            write_error_log("Failed to resolve module config path for watchdog.json");
            return false;
        }

        obs_data_t* data = settings_to_obs_data(s);
        const bool ok = obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
        obs_data_release(data);

        if (!ok)
            write_error_log("Failed to save settings to '%s'", path.c_str());

        return ok;
    }

    static std::vector<std::string> split_lines(const std::string& text)
    {
        std::vector<std::string> out;
        std::string cur;

        for (char ch : text) {
            if (ch == '\r')
                continue;
            if (ch == '\n') {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
                continue;
            }
            cur.push_back(ch);
        }
        if (!cur.empty())
            out.push_back(cur);

        for (auto& s : out) {
            auto not_space = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
            s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        }

        out.erase(std::remove_if(out.begin(), out.end(),
            [](const std::string& s) { return s.empty(); }),
            out.end());

        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    class WatchdogController {
    public:
        explicit WatchdogController(const WatchdogSettings& initial)
        {
            update(initial);
        }

        ~WatchdogController()
        {
            restore_hidden_items();
        }

        void update(const WatchdogSettings& new_settings)
        {
            std::lock_guard<std::mutex> lock(mutex);

            settings = new_settings;

            for (auto it = states.begin(); it != states.end();) {
                if (std::find(settings.watched_sources.begin(),
                    settings.watched_sources.end(),
                    it->first) == settings.watched_sources.end())
                    it = states.erase(it);
                else
                    ++it;
            }

            if (!settings.watched_sources.empty() && next_index >= settings.watched_sources.size())
                next_index = 0;
        }

        WatchdogSettings snapshot_settings() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return settings;
        }

        void tick(float)
        {
            maybe_restore_hidden_items();

            std::string target;

            {
                std::lock_guard<std::mutex> lock(mutex);

                if (pending_screenshot && now_ms() - pending_started_ms > kPendingScreenshotTimeoutMs) {
                    write_error_log("Screenshot timeout for source '%s'. Pending state cleared.",
                        pending_source.c_str());
                    pending_screenshot = false;
                    pending_source.clear();
                }

                if (!settings.enabled || pending_screenshot || pending_restore ||
                    settings.watched_sources.empty())
                    return;

                if (now_ms() - last_reset_ms < settings.cooldown_ms)
                    return;

                const size_t n = settings.watched_sources.size();
                for (size_t offset = 0; offset < n; ++offset) {
                    const size_t idx = (next_index + offset) % n;
                    const std::string& name = settings.watched_sources[idx];
                    auto it = states.find(name);
                    const uint64_t last = (it != states.end()) ? it->second.last_check_ms : 0;

                    if (now_ms() - last >= settings.interval_ms) {
                        target = name;
                        next_index = (idx + 1) % n;
                        pending_screenshot = true;
                        pending_source = name;
                        pending_started_ms = now_ms();
                        break;
                    }
                }
            }

            if (target.empty())
                return;

            if (!source_is_visible_in_active_scene(target)) {
                std::lock_guard<std::mutex> lock(mutex);
                pending_screenshot = false;
                pending_source.clear();
                states[target].last_check_ms = now_ms();
                return;
            }

            obs_source_t* source = obs_get_source_by_name(target.c_str());
            if (!source) {
                write_error_log("Configured source '%s' was not found in OBS.", target.c_str());
                std::lock_guard<std::mutex> lock(mutex);
                pending_screenshot = false;
                pending_source.clear();
                states[target].last_check_ms = now_ms();
                return;
            }

            const uint32_t w = obs_source_get_width(source);
            const uint32_t h = obs_source_get_height(source);

            if (w == 0 || h == 0) {
                bool should_report = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    SourceState& state = states[target];
                    state.last_check_ms = now_ms();
                    state.invalid_size_hits += 1;
                    if (state.invalid_size_hits >= settings.invalid_size_hits_required)
                        should_report = true;
                    pending_screenshot = false;
                    pending_source.clear();
                }

                if (should_report) {
                    write_error_log("Source '%s' is not operational or not producing visible video. Invalid size: %ux%u.",
                        target.c_str(), w, h);
                }

                obs_source_release(source);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                states[target].invalid_size_hits = 0;
            }

            obs_frontend_take_source_screenshot(source);
            obs_source_release(source);
        }

        void handle_screenshot_taken(const std::string& path)
        {
            std::string source_to_process;
            WatchdogSettings current;

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!pending_screenshot)
                    return;
                pending_screenshot = false;
                source_to_process = pending_source;
                pending_source.clear();
                current = settings;
            }

            if (source_to_process.empty())
                return;

            std::vector<uint8_t> sig;
            if (!load_signature(path, sig)) {
                write_error_log("Failed to decode screenshot for source '%s'.", source_to_process.c_str());
                return;
            }

            const int avg = signature_average(sig);
            const int range = signature_range(sig);
            const bool blank = is_blank_frame(sig, current.blank_dark_threshold, current.blank_bright_threshold);
            const bool flat = is_flat_frame(sig, current.flat_range_threshold);

            bool should_reset = false;
            int diff_value = 255;
            int unchanged_hits = 0;
            int blank_hits = 0;
            int flat_hits = 0;

            {
                std::lock_guard<std::mutex> lock(mutex);
                SourceState& state = states[source_to_process];
                state.last_check_ms = now_ms();

                if (!state.signature.empty()) {
                    diff_value = mean_abs_diff(state.signature, sig);
                    if (diff_value <= current.unchanged_threshold)
                        state.unchanged_hits += 1;
                    else
                        state.unchanged_hits = 0;
                }
                else {
                    state.unchanged_hits = 0;
                }

                if (blank)
                    state.blank_hits += 1;
                else
                    state.blank_hits = 0;

                if (flat)
                    state.flat_hits += 1;
                else
                    state.flat_hits = 0;

                state.signature = std::move(sig);
                unchanged_hits = state.unchanged_hits;
                blank_hits = state.blank_hits;
                flat_hits = state.flat_hits;

                should_reset = (unchanged_hits >= current.unchanged_hits_required) ||
                    (blank_hits >= current.blank_hits_required) ||
                    (flat_hits >= current.flat_hits_required);
            }

            if (should_reset) {
                write_reset_log(
                    "Reset condition met for source '%s' | diff=%d | avg=%d | range=%d | unchanged_hits=%d | blank_hits=%d | flat_hits=%d",
                    source_to_process.c_str(), diff_value, avg, range, unchanged_hits, blank_hits, flat_hits);
                reset_source(source_to_process);
            }
        }

    private:
        mutable std::mutex mutex;
        WatchdogSettings settings = default_settings();

        bool pending_screenshot = false;
        bool pending_restore = false;
        uint64_t last_reset_ms = 0;
        uint64_t pending_started_ms = 0;
        uint64_t restore_due_ms = 0;
        std::string pending_source;
        std::vector<HiddenSceneItem> hidden_items;
        std::unordered_map<std::string, SourceState> states;
        size_t next_index = 0;

        bool source_is_visible_in_active_scene(const std::string& name)
        {
            obs_source_t* scene_source = obs_frontend_get_current_scene();
            if (!scene_source)
                return false;

            obs_scene_t* scene = obs_scene_from_source(scene_source);
            bool visible = false;

            if (scene) {
                obs_sceneitem_t* item = obs_scene_find_source_recursive(scene, name.c_str());
                visible = item && obs_sceneitem_visible(item);
            }

            obs_source_release(scene_source);
            return visible;
        }

        static int mean_abs_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 255;

            uint64_t total = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                const int d = (int)a[i] - (int)b[i];
                total += (uint64_t)std::abs(d);
            }
            return (int)(total / a.size());
        }

        static bool load_signature(const std::string& path, std::vector<uint8_t>& out)
        {
            QImage image(QString::fromStdString(path));
            if (image.isNull())
                return false;

            QImage rgb = image.convertToFormat(QImage::Format_RGB32);
            if (rgb.isNull())
                return false;

            const int w = rgb.width();
            const int h = rgb.height();
            if (w <= 0 || h <= 0)
                return false;

            out.resize(64 * 64);

            for (int y = 0; y < 64; ++y) {
                const int sy = std::min(h - 1, (y * h) / 64);
                for (int x = 0; x < 64; ++x) {
                    const int sx = std::min(w - 1, (x * w) / 64);

                    const QColor c = rgb.pixelColor(sx, sy);
                    out[(size_t)y * 64 + (size_t)x] = (uint8_t)(
                        ((uint32_t)c.red() + (uint32_t)c.green() + (uint32_t)c.blue()) / 3U);
                }
            }

            return true;
        }

        static int signature_average(const std::vector<uint8_t>& sig)
        {
            if (sig.empty())
                return 0;

            uint64_t sum = 0;
            for (uint8_t v : sig)
                sum += v;

            return (int)(sum / sig.size());
        }

        static int signature_range(const std::vector<uint8_t>& sig)
        {
            if (sig.empty())
                return 255;

            int min_v = 255;
            int max_v = 0;

            for (uint8_t v : sig) {
                if ((int)v < min_v)
                    min_v = (int)v;
                if ((int)v > max_v)
                    max_v = (int)v;
            }

            return max_v - min_v;
        }

        static bool is_blank_frame(const std::vector<uint8_t>& sig, int dark_threshold, int bright_threshold)
        {
            const int avg = signature_average(sig);
            return avg <= dark_threshold || avg >= bright_threshold;
        }

        static bool is_flat_frame(const std::vector<uint8_t>& sig, int range_threshold)
        {
            return signature_range(sig) <= range_threshold;
        }

        void reset_source(const std::string& source_name)
        {
            blog(LOG_WARNING, "[obs-freeze-watchdog] ENTER reset '%s'", source_name.c_str());

            obs_source_t* source = obs_get_source_by_name(source_name.c_str());
            if (!source) {
                write_error_log("Reset failed: source '%s' was not found.", source_name.c_str());
                return;
            }

            obs_source_media_restart(source);

            std::vector<HiddenSceneItem> local_hidden;
            obs_frontend_source_list scenes = {};
            obs_frontend_get_scenes(&scenes);

            for (size_t i = 0; i < scenes.sources.num; ++i) {
                obs_source_t* scene_source = scenes.sources.array[i];
                obs_scene_t* scene = obs_scene_from_source(scene_source);
                if (!scene)
                    continue;

                obs_sceneitem_t* item = obs_scene_find_source_recursive(scene, source_name.c_str());
                if (!item)
                    continue;
                if (!obs_sceneitem_visible(item))
                    continue;

                obs_sceneitem_addref(item);
                obs_sceneitem_set_visible(item, false);
                local_hidden.push_back({ item });
            }

            obs_frontend_source_list_free(&scenes);
            obs_source_release(source);

            if (local_hidden.empty()) {
                write_error_log("Reset fallback for source '%s' did not find any visible scene items to hide.",
                    source_name.c_str());
                return;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                pending_restore = true;
                last_reset_ms = now_ms();
                restore_due_ms = now_ms() + settings.hide_duration_ms;
                hidden_items = std::move(local_hidden);

                auto it = states.find(source_name);
                if (it != states.end()) {
                    it->second.signature.clear();
                    it->second.unchanged_hits = 0;
                    it->second.blank_hits = 0;
                    it->second.flat_hits = 0;
                    it->second.invalid_size_hits = 0;
                }
            }
        }

        void maybe_restore_hidden_items()
        {
            std::vector<HiddenSceneItem> local;

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!pending_restore)
                    return;
                if (now_ms() < restore_due_ms)
                    return;

                local.swap(hidden_items);
                pending_restore = false;
                restore_due_ms = 0;
            }

            for (auto& entry : local) {
                if (entry.item) {
                    obs_sceneitem_set_visible(entry.item, true);
                    obs_sceneitem_release(entry.item);
                }
            }

            write_reset_log("Hidden scene items restored.");
        }

        void restore_hidden_items()
        {
            std::vector<HiddenSceneItem> local;

            {
                std::lock_guard<std::mutex> lock(mutex);
                local.swap(hidden_items);
                pending_restore = false;
                restore_due_ms = 0;
            }

            for (auto& entry : local) {
                if (entry.item) {
                    obs_sceneitem_set_visible(entry.item, true);
                    obs_sceneitem_release(entry.item);
                }
            }
        }
    };

    static std::unique_ptr<WatchdogController> g_controller;
    static QPointer<QDialog> g_dialog;

    class WatchdogDialog : public QDialog {
    public:
        explicit WatchdogDialog(QWidget* parent = nullptr) : QDialog(parent)
        {
            setWindowTitle("Freeze Watchdog Settings");
            resize(640, 700);

            auto* main = new QVBoxLayout(this);

            enabled = new QCheckBox("Enabled", this);
            main->addWidget(enabled);

            auto* sourcesLabel = new QLabel("Watched Sources (one source name per line)", this);
            main->addWidget(sourcesLabel);

            sources = new QPlainTextEdit(this);
            sources->setPlaceholderText("Media Source 1\nBrowser Source\nCamera A");
            main->addWidget(sources, 1);

            auto* form = new QFormLayout();
            interval_ms = spin(this, 1000, 600000, 10000, 500);
            unchanged_hits_required = spin(this, 1, 20, 4, 1);
            unchanged_threshold = spin(this, 0, 255, 0, 1);
            blank_hits_required = spin(this, 1, 20, 2, 1);
            blank_dark_threshold = spin(this, 0, 100, 5, 1);
            blank_bright_threshold = spin(this, 155, 255, 250, 1);
            flat_hits_required = spin(this, 1, 20, 2, 1);
            flat_range_threshold = spin(this, 0, 50, 3, 1);
            invalid_size_hits_required = spin(this, 1, 20, 3, 1);
            hide_duration_ms = spin(this, 100, 10000, 1500, 100);
            cooldown_seconds = spin(this, 1, 600, 45, 1);

            form->addRow("Check interval per source (ms)", interval_ms);
            form->addRow("Unchanged hits before reset", unchanged_hits_required);
            form->addRow("Allowed mean diff (0 = exact)", unchanged_threshold);
            form->addRow("Blank hits before reset", blank_hits_required);
            form->addRow("Blank dark threshold", blank_dark_threshold);
            form->addRow("Blank bright threshold", blank_bright_threshold);
            form->addRow("Flat hits before reset", flat_hits_required);
            form->addRow("Flat range threshold", flat_range_threshold);
            form->addRow("Invalid size hits before error", invalid_size_hits_required);
            form->addRow("Hide duration (ms)", hide_duration_ms);
            form->addRow("Cooldown after reset (s)", cooldown_seconds);

            main->addLayout(form);

            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
            connect(buttons, &QDialogButtonBox::accepted, this, [this]() { on_save(); });
            connect(buttons, &QDialogButtonBox::rejected, this, [this]() { close(); });
            main->addWidget(buttons);

            reload_from_controller();
        }

    private:
        QCheckBox* enabled = nullptr;
        QPlainTextEdit* sources = nullptr;
        QSpinBox* interval_ms = nullptr;
        QSpinBox* unchanged_hits_required = nullptr;
        QSpinBox* unchanged_threshold = nullptr;
        QSpinBox* blank_hits_required = nullptr;
        QSpinBox* blank_dark_threshold = nullptr;
        QSpinBox* blank_bright_threshold = nullptr;
        QSpinBox* flat_hits_required = nullptr;
        QSpinBox* flat_range_threshold = nullptr;
        QSpinBox* invalid_size_hits_required = nullptr;
        QSpinBox* hide_duration_ms = nullptr;
        QSpinBox* cooldown_seconds = nullptr;

        static QSpinBox* spin(QWidget* parent, int min, int max, int value, int step)
        {
            auto* box = new QSpinBox(parent);
            box->setRange(min, max);
            box->setSingleStep(step);
            box->setValue(value);
            return box;
        }

        void reload_from_controller()
        {
            WatchdogSettings s = g_controller ? g_controller->snapshot_settings() : load_settings();

            enabled->setChecked(s.enabled);

            QStringList lines;
            for (const auto& src : s.watched_sources)
                lines << QString::fromStdString(src);
            sources->setPlainText(lines.join("\n"));

            interval_ms->setValue((int)s.interval_ms);
            unchanged_hits_required->setValue(s.unchanged_hits_required);
            unchanged_threshold->setValue(s.unchanged_threshold);
            blank_hits_required->setValue(s.blank_hits_required);
            blank_dark_threshold->setValue(s.blank_dark_threshold);
            blank_bright_threshold->setValue(s.blank_bright_threshold);
            flat_hits_required->setValue(s.flat_hits_required);
            flat_range_threshold->setValue(s.flat_range_threshold);
            invalid_size_hits_required->setValue(s.invalid_size_hits_required);
            hide_duration_ms->setValue((int)s.hide_duration_ms);
            cooldown_seconds->setValue((int)(s.cooldown_ms / 1000ULL));
        }

        void on_save()
        {
            WatchdogSettings s;
            s.enabled = enabled->isChecked();
            s.watched_sources = split_lines(sources->toPlainText().toStdString());
            s.interval_ms = (uint64_t)interval_ms->value();
            s.unchanged_hits_required = unchanged_hits_required->value();
            s.unchanged_threshold = unchanged_threshold->value();
            s.blank_hits_required = blank_hits_required->value();
            s.blank_dark_threshold = blank_dark_threshold->value();
            s.blank_bright_threshold = blank_bright_threshold->value();
            s.flat_hits_required = flat_hits_required->value();
            s.flat_range_threshold = flat_range_threshold->value();
            s.invalid_size_hits_required = invalid_size_hits_required->value();
            s.hide_duration_ms = (uint64_t)hide_duration_ms->value();
            s.cooldown_ms = (uint64_t)cooldown_seconds->value() * 1000ULL;

            save_settings(s);
            if (g_controller)
                g_controller->update(s);

            write_reset_log("Settings updated from Tools dialog.");
            accept();
        }
    };

    static void watchdog_tick(void*, float seconds)
    {
        if (g_controller)
            g_controller->tick(seconds);
    }

    static void on_frontend_event(enum obs_frontend_event event, void*)
    {
        if (event != OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN)
            return;
        if (!g_controller)
            return;

        char* last = obs_frontend_get_last_screenshot();
        if (!last)
            return;

        std::string path = last;
        bfree(last);

        g_controller->handle_screenshot_taken(path);
        std::remove(path.c_str());
    }

    static void show_settings_dialog(void* data)
    {
        (void)data;

        QWidget* parent = static_cast<QWidget*>(obs_frontend_get_main_window());

        if (!g_dialog) {
            g_dialog = new WatchdogDialog(parent);
            g_dialog->setAttribute(Qt::WA_DeleteOnClose, true);
            QObject::connect(g_dialog, &QObject::destroyed, []() { g_dialog = nullptr; });
            g_dialog->show();
        }
        else {
            g_dialog->raise();
            g_dialog->activateWindow();
        }
    }

    static void ensure_default_config_exists()
    {
        const std::string path = module_config_file_path("watchdog.json");
        if (path.empty())
            return;

        std::ifstream in(path);
        if (in.good())
            return;

        save_settings(default_settings());
    }

} // namespace

bool obs_module_load(void)
{
    g_reset_log.open("freeze_watchdog_resets.log", std::ios::out | std::ios::app);
    g_error_log.open("freeze_watchdog_errors.log", std::ios::out | std::ios::app);

    ensure_default_config_exists();

    WatchdogSettings settings = load_settings();
    g_controller = std::make_unique<WatchdogController>(settings);

    obs_add_tick_callback(watchdog_tick, nullptr);
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    obs_frontend_add_tools_menu_item("Freeze Watchdog Settings", show_settings_dialog, nullptr);

    write_reset_log("Frontend plugin loaded.");
    return true;
}

void obs_module_unload(void)
{
    if (g_dialog)
        g_dialog->close();

    obs_remove_tick_callback(watchdog_tick, nullptr);
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);

    g_controller.reset();

    write_reset_log("Frontend plugin unloaded.");

    if (g_reset_log.is_open())
        g_reset_log.close();
    if (g_error_log.is_open())
        g_error_log.close();
}