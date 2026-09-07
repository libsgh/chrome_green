<template>
  <div class="card relative">
    <span class="restart-chip absolute top-4 right-4 z-10">{{ t("restart_required") }}</span>
    <div class="card-content pb-16 pt-12">
<!-- Top horizontal group tabs -->
      <nav
        class="flex gap-1 overflow-x-auto border-b border-[hsl(var(--border))] -mx-1 px-1 mb-6">
        <button
          v-for="g in groups"
          :key="g.id"
          type="button"
          :class="['subtab', activeGroup === g.id ? 'active' : '']"
          @click="activeGroup = g.id">
          {{ t(g.label) }}
        </button>
      </nav>

<!-- Content (active group only) -->
      <div class="min-w-0">
        <!-- Paths & Data -->
        <section v-show="activeGroup === 'paths'" class="space-y-5">
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("data_dir") }}</label>
              <p class="setting-desc">{{ t("data_dir_desc") }}</p>
            </div>
            <div class="w-80 max-w-full">
              <input
                v-model="settings.data_dir"
                @blur="saveSettings"
                class="input"
                placeholder="%app%\..\Data" />
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("cache_dir") }}</label>
              <p class="setting-desc">{{ t("cache_dir_desc") }}</p>
            </div>
            <div class="w-80 max-w-full">
              <input
                v-model="settings.cache_dir"
                @blur="saveSettings"
                class="input"
                placeholder="%app%\..\Cache" />
            </div>
          </div>
        </section>

        <!-- Startup Behavior -->
        <section v-show="activeGroup === 'startup'" class="space-y-5">
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("command_line") }}</label>
              <p class="setting-desc">{{ t("command_line_desc") }}</p>
            </div>
            <div class="w-80 max-w-full">
              <input
                v-model="settings.command_line"
                @blur="saveSettings"
                class="input"
                placeholder="--disable-features=..." />
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("launch_on_startup") }}</label>
              <p class="setting-desc">{{ t("launch_on_startup_desc") }}</p>
            </div>
            <div class="w-80 max-w-full">
              <input
                v-model="settings.launch_on_startup"
                @blur="saveSettings"
                class="input" />
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("launch_on_exit") }}</label>
              <p class="setting-desc">{{ t("launch_on_exit_desc") }}</p>
            </div>
            <div class="w-80 max-w-full">
              <input
                v-model="settings.launch_on_exit"
                @blur="saveSettings"
                class="input" />
            </div>
          </div>
        </section>

        <!-- Hotkeys -->
        <section v-show="activeGroup === 'hotkeys'" class="space-y-5">
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("translate_key") }}</label>
              <p class="setting-desc">{{ t("translate_key_desc") }}</p>
            </div>
            <div class="w-64 max-w-full">
              <KeyInput
                v-model="settings.translate_key"
                :placeholder="t('translate_key_desc')"
                @change="saveSettings" />
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("boss_key") }}</label>
              <p class="setting-desc">{{ t("boss_key_desc") }}</p>
            </div>
            <div class="w-64 max-w-full">
              <KeyInput
                v-model="settings.boss_key"
                :placeholder="t('boss_key_desc')"
                @change="saveSettings" />
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("action_open_new_window") }}</label>
              <p class="setting-desc">{{ t("action_open_new_window_desc") }}</p>
            </div>
            <div class="w-64 max-w-full">
              <KeyInput
                v-model="settings.open_new_window"
                :placeholder="t('action_open_new_window_ph')"
                @change="saveSettings" />
            </div>
          </div>
          <div class="setting-row !items-start">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("action_open_url_group") }}</label>
              <p class="setting-desc">{{ t("action_open_url_group_desc") }}</p>
            </div>
            <div class="w-80 max-w-full space-y-2">
              <KeyInput
                v-model="settings.open_url_group"
                :placeholder="t('action_open_url_group_ph')"
                @change="saveSettings" />
              <textarea
                v-model="settings.url_group"
                spellcheck="false"
                @blur="saveSettings"
                class="input min-h-[88px] w-full resize-y font-mono text-xs leading-relaxed"
                :placeholder="t('url_group_desc')"></textarea>
            </div>
          </div>
        </section>

        <!-- Privacy & Security -->
        <section v-show="activeGroup === 'privacy'" class="space-y-5">
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("show_password") }}</label>
              <p class="setting-desc">{{ t("show_password_desc") }}</p>
            </div>
            <div
              :class="['switch-track', settings.show_password ? 'on' : 'off']"
              @click="toggle('show_password')"
              role="switch"
              :aria-checked="settings.show_password">
              <span class="switch-thumb"></span>
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("win32k") }}</label>
              <p class="setting-desc">{{ t("win32k_desc") }}</p>
            </div>
            <div
              :class="['switch-track', settings.win32k ? 'on' : 'off']"
              @click="toggle('win32k')"
              role="switch"
              :aria-checked="settings.win32k">
              <span class="switch-thumb"></span>
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("ignore_policies") }}</label>
              <p class="setting-desc">{{ t("ignore_policies_desc") }}</p>
            </div>
            <div
              :class="[
                'switch-track',
                settings.ignore_policies ? 'on' : 'off',
              ]"
              @click="toggle('ignore_policies')"
              role="switch"
              :aria-checked="settings.ignore_policies">
              <span class="switch-thumb"></span>
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{
                t("suppress_false_upgrade_notification")
              }}</label>
              <p class="setting-desc">{{
                t("suppress_false_upgrade_notification_desc")
              }}</p>
            </div>
            <div
              :class="[
                'switch-track',
                settings.suppress_false_upgrade_notification ? 'on' : 'off',
              ]"
              @click="toggle('suppress_false_upgrade_notification')"
              role="switch"
              :aria-checked="
                settings.suppress_false_upgrade_notification
              ">
              <span class="switch-thumb"></span>
            </div>
          </div>
        </section>

        <!-- Key Mappings (was "Advanced") -->
        <section v-show="activeGroup === 'advanced'" class="space-y-5">
          <div class="setting-row !items-start">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("keymapping") }}</label>
              <p class="setting-desc">{{ t("keymapping_desc") }}</p>
            </div>
            <div class="w-full space-y-2">
              <button
                type="button"
                class="keymap-toggle"
                @click="showKeymapping = !showKeymapping">
                <Icon
                  :name="showKeymapping ? 'chevronDown' : 'chevronRight'"
                  :size="14" />
                <span class="flex-1 text-left">{{ t("keymapping") }}</span>
              </button>

              <div v-if="showKeymapping" class="space-y-2 pt-1">
                <div
                  v-for="(m, idx) in keyMappings"
                  :key="idx"
                  class="flex items-center gap-2">
                  <KeyInput
                    v-model="m.src"
                    class="flex-1"
                    :placeholder="t('keymapping_src')" />
                  <span class="text-[hsl(var(--muted-foreground))] font-medium">=</span>
                  <KeyInput
                    v-model="m.dst"
                    class="flex-1"
                    :placeholder="t('keymapping_target')" />
                  <button
                    class="icon-btn shrink-0"
                    @click="removeMapping(idx)"
                    :title="t('keymapping_remove')">
                    <Icon name="close" :size="16" />
                  </button>
                </div>
                <button class="btn btn-outline text-xs" @click="addMapping">
                  {{ t("keymapping_add") }}
                </button>
                <p
                  class="text-xs text-[hsl(var(--muted-foreground))] leading-relaxed">
                  {{ t("keymapping_hint") }}
                </p>
              </div>
            </div>
          </div>
        </section>

        <!-- Other (was "Debug Log", now last) -->
        <section v-show="activeGroup === 'debug'" class="space-y-5">
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label flex items-center gap-2">
                <span>{{ t("fix_taskbar_menu") }}</span>
                <span class="badge badge-experiment text-xs">{{
                  t("experimental")
                }}</span>
              </label>
              <p class="setting-desc">{{ t("fix_taskbar_menu_desc") }}</p>
            </div>
            <div
              :class="['switch-track', settings.fix_taskbar_menu ? 'on' : 'off']"
              @click="toggle('fix_taskbar_menu')"
              role="switch"
              :aria-checked="settings.fix_taskbar_menu">
              <span class="switch-thumb"></span>
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("debug_log") }}</label>
              <p class="setting-desc">{{ t("debug_log_desc") }}</p>
            </div>
            <div
              :class="['switch-track', settings.debug_log ? 'on' : 'off']"
              @click="toggle('debug_log')"
              role="switch"
              :aria-checked="settings.debug_log">
              <span class="switch-thumb"></span>
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("suppress_cmdline_warning") }}</label>
              <p class="setting-desc">{{ t("suppress_cmdline_warning_desc") }}</p>
            </div>
            <div
              :class="['switch-track', settings.suppress_cmdline_warning ? 'on' : 'off']"
              @click="toggle('suppress_cmdline_warning')"
              role="switch"
              :aria-checked="settings.suppress_cmdline_warning">
              <span class="switch-thumb"></span>
            </div>
          </div>
          <div class="setting-row">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("open_config_after_update") }}</label>
              <p class="setting-desc">{{ t("open_config_after_update_desc") }}</p>
            </div>
            <div
              :class="['switch-track', settings.open_config_after_update ? 'on' : 'off']"
              @click="toggle('open_config_after_update')"
              role="switch"
              :aria-checked="settings.open_config_after_update">
              <span class="switch-thumb"></span>
            </div>
          </div>
          <div class="setting-row !items-start">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("tool_shortcut_btn") }}</label>
              <p class="setting-desc">{{ t("tool_shortcut_desc") }}</p>
            </div>
            <div class="flex flex-col items-end gap-2">
              <button
                class="btn btn-outline"
                @click="createShortcut"
                :disabled="tools.desktop_shortcut || shortcutBusy"
                :title="tools.desktop_shortcut ? t('tool_shortcut_title') : ''">
                <span
                  v-if="shortcutBusy"
                  class="inline-block w-3.5 h-3.5 border-2 border-current/30 border-t-current rounded-full animate-spin mr-1.5 align-middle -mt-0.5"></span>
                {{ tools.desktop_shortcut ? t("tool_status_created") : t("tool_shortcut_btn") }}
              </button>
              <span v-if="toolMessage" class="text-xs" :class="toolMessageClass">
                {{ toolMessage }}
              </span>
            </div>
          </div>
          <div class="setting-row !items-start">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("tool_clean_title") }}</label>
              <p class="setting-desc">{{ t("tool_clean_desc") }}</p>
              <p
                class="text-xs text-[hsl(var(--muted-foreground))] mt-1 font-mono break-all">
                {{ t("tool_clean_path") }}
              </p>
              <p
                class="text-xs mt-1"
                :class="
                  chromeData.empty
                    ? 'text-[hsl(var(--muted-foreground))]'
                    : 'text-[hsl(var(--warning))]'
                ">
                {{
                  chromeData.empty
                    ? t("tool_clean_status_empty")
                    : t("tool_clean_status_exists").replace(
                        "{n}",
                        chromeData.entry_count
                      )
                }}
              </p>
              <p
                v-if="chromeDataMessage"
                class="text-xs mt-1"
                :class="
                  chromeDataMessageOk
                    ? 'text-[hsl(var(--success))]'
                    : 'text-[hsl(var(--destructive))]'
                ">
                {{ chromeDataMessage }}
              </p>
            </div>
            <div class="flex flex-col items-end gap-2">
              <button
                class="btn btn-outline"
                :disabled="chromeData.empty || chromeDataBusy"
                @click="cleanChromeData">
                <span
                  v-if="chromeDataBusy"
                  class="inline-block w-3.5 h-3.5 border-2 border-current/30 border-t-current rounded-full animate-spin mr-1.5 align-middle -mt-0.5"></span>
                {{ t("tool_clean_btn") }}
              </button>
            </div>
          </div>
        </section>
      </div>
    </div>

<!-- Save button pinned to the card's bottom-right -->
    <button
      class="btn btn-primary absolute bottom-4 right-4 z-10 shadow-lg gap-1.5"
      @click="saveSettings(false, true)">
      <Icon name="save" :size="15" />
      {{ t("save_changes") }}
    </button>
  </div>
</template>

<script setup>
import { ref } from "vue";
import Icon from "../Icon.vue";
import KeyInput from "../KeyInput.vue";
import { useStore } from "../../store.js";
import { t } from "../../i18n.js";

const {
  settings,
  keyMappings,
  saveSettings,
  toggle,
  addMapping,
  removeMapping,
  tools,
  shortcutBusy,
  toolMessage,
  toolMessageClass,
  createShortcut,
  chromeData,
  chromeDataBusy,
  chromeDataMessage,
  chromeDataMessageOk,
  cleanChromeData,
} = useStore();

// Top horizontal group tabs (order = display order).
// "调试日志" renamed to "其他" (grp_other) and moved to the end;
// "高级" renamed to "按键映射" (grp_advanced / Key Mappings).
const groups = [
  { id: "paths", label: "grp_paths" },
  { id: "startup", label: "grp_startup" },
  { id: "hotkeys", label: "grp_hotkeys" },
  { id: "privacy", label: "grp_privacy" },
  { id: "advanced", label: "grp_advanced" },
  { id: "debug", label: "grp_other" },
];
const activeGroup = ref("paths");

// Key mappings editor is collapsed by default to keep the tab tidy.
const showKeymapping = ref(false);
</script>
