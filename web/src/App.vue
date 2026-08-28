<template>
  <div :class="['min-h-screen bg-[hsl(var(--background))]', themeClass]">
    <!-- Header -->
    <header
      class="border-b border-[hsl(var(--border))] bg-[hsl(var(--card))] sticky top-0 z-20">
      <div class="max-w-3xl mx-auto px-6">
        <!-- Title row -->
        <div class="py-3 flex items-center gap-3">
          <div
            class="w-8 h-8 rounded-lg bg-[hsl(var(--primary))] text-[hsl(var(--primary-foreground))] flex items-center justify-center shrink-0">
            <Icon name="logo" :size="18" stroke="currentColor" :stroke-width="2.2" />
          </div>
          <div class="flex-1">
            <h1 class="text-sm font-semibold">{{ t("title") }}</h1>
            <p class="text-[11px] text-[hsl(var(--muted-foreground))]">
              {{ t("subtitle") }}
            </p>
          </div>

          <!-- Language dropdown -->
          <div class="relative" ref="langDropdownEl">
            <button
              class="icon-btn"
              @click="toggleLangDropdown"
              :title="t('language')">
              <Icon name="globe" :size="16" />
            </button>
            <div
              v-if="showLangDropdown"
              class="dropdown-menu right-0 top-full mt-1">
              <button
                :class="['dropdown-item', langMode === 'auto' ? 'active' : '']"
                @click="setLangMode('auto')">
                <span>{{ t("lang_auto") }}</span>
                <Icon
                  v-if="langMode === 'auto'"
                  name="check"
                  class-name="check-icon"
                  :size="16"
                  :stroke-width="2" />
              </button>
              <button
                :class="['dropdown-item', langMode === 'en' ? 'active' : '']"
                @click="setLangMode('en')">
                <span>English</span>
                <Icon
                  v-if="langMode === 'en'"
                  name="check"
                  class-name="check-icon"
                  :size="16"
                  :stroke-width="2" />
              </button>
              <button
                :class="['dropdown-item', langMode === 'zh-CN' ? 'active' : '']"
                @click="setLangMode('zh-CN')">
                <span>简体中文</span>
                <Icon
                  v-if="langMode === 'zh-CN'"
                  name="check"
                  class-name="check-icon"
                  :size="16"
                  :stroke-width="2" />
              </button>
            </div>
          </div>

          <!-- Theme dropdown -->
          <div class="relative" ref="themeDropdownEl">
            <button
              class="icon-btn"
              @click="toggleThemeDropdown"
              :title="t('theme')">
              <Icon v-if="themeMode === 'dark'" name="moon" :size="16" />
              <Icon
                v-else-if="themeMode === 'auto'"
                name="systemTheme"
                :size="16" />
              <Icon v-else name="sun" :size="16" />
            </button>
            <div
              v-if="showThemeDropdown"
              class="dropdown-menu right-0 top-full mt-1">
              <button
                :class="['dropdown-item', themeMode === 'auto' ? 'active' : '']"
                @click="setThemeMode('auto')">
                <span class="flex items-center gap-2">
                  <Icon name="systemTheme" :size="14" />
                  <span>{{ t("theme_auto") }}</span>
                </span>
                <Icon
                  v-if="themeMode === 'auto'"
                  name="check"
                  class-name="check-icon"
                  :size="16"
                  :stroke-width="2" />
              </button>
              <button
                :class="[
                  'dropdown-item',
                  themeMode === 'light' ? 'active' : '',
                ]"
                @click="setThemeMode('light')">
                <span class="flex items-center gap-2">
                  <Icon name="sun" :size="14" />
                  <span>{{ t("theme_light") }}</span>
                </span>
                <Icon
                  v-if="themeMode === 'light'"
                  name="check"
                  class-name="check-icon"
                  :size="16"
                  :stroke-width="2" />
              </button>
              <button
                :class="['dropdown-item', themeMode === 'dark' ? 'active' : '']"
                @click="setThemeMode('dark')">
                <span class="flex items-center gap-2">
                  <Icon name="moon" :size="14" />
                  <span>{{ t("theme_dark") }}</span>
                </span>
                <Icon
                  v-if="themeMode === 'dark'"
                  name="check"
                  class-name="check-icon"
                  :size="16"
                  :stroke-width="2" />
              </button>
            </div>
          </div>

          <!-- GitHub icon -->
          <a
            :href="githubUrl"
            target="_blank"
            class="icon-btn"
            :title="t('github')">
            <Icon name="github" :size="16" />
          </a>
        </div>
      </div>
    </header>

    <!-- Main Content -->
    <main class="max-w-3xl mx-auto px-6 py-8 space-y-6">
      <!-- Navigation tabs -->
      <nav class="flex gap-1 pb-1 -mx-1 overflow-x-auto">
        <button
          v-for="tab in tabs"
          :key="tab.id"
          :class="['nav-tab', activeTab === tab.id ? 'active' : '']"
          @click="activeTab = tab.id">
          <span class="tab-icon" aria-hidden="true">
            <Icon :name="tab.icon" :size="15" />
          </span>
          <span class="nav-label leading-none">{{ t(tab.label) }}</span>
        </button>
      </nav>

      <!-- Active tab component -->
      <component :is="tabMap[activeTab]" />
    </main>

    <!-- Toast notification -->
    <transition name="toast-fade">
      <div v-if="toast.show" :class="['toast', 'toast-' + toast.type]">
        <Icon :name="toast.type === 'success' ? 'check' : 'close'" :size="16" />
        <span>{{ toast.message }}</span>
      </div>
    </transition>
  </div>
</template>

<script setup>
import { onBeforeUnmount, onMounted, watch } from "vue";
import Icon from "./components/Icon.vue";
import { t } from "./i18n.js";
import { useStore } from "./store.js";
import StatusTab from "./components/tabs/StatusTab.vue";
import SettingsTab from "./components/tabs/SettingsTab.vue";
import ProxyTab from "./components/tabs/ProxyTab.vue";
import LogsTab from "./components/tabs/LogsTab.vue";
import OtherTab from "./components/tabs/OtherTab.vue";
import TabsTab from "./components/tabs/TabsTab.vue";
import ResolverTab from "./components/tabs/ResolverTab.vue";

const {
  themeMode,
  langMode,
  showThemeDropdown,
  showLangDropdown,
  activeTab,
  tabs,
  toast,
  themeClass,
  githubUrl,
  status,
  setThemeMode,
  setLangMode,
  toggleThemeDropdown,
  toggleLangDropdown,
  initApp,
  disposeApp,
  updatePolling,
} = useStore();

const tabMap = {
  status: StatusTab,
  settings: SettingsTab,
  proxy: ProxyTab,
  resolver: ResolverTab,
  tabs: TabsTab,
  logs: LogsTab,
  other: OtherTab,
};

// Logs / tools tabs poll only while visible.
watch([activeTab, () => status.state], updatePolling, { immediate: true });

onMounted(() => {
  initApp();
});
onBeforeUnmount(() => {
  disposeApp();
});
</script>
