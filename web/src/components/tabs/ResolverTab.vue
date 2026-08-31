<template>
  <div class="card relative">
    <span class="restart-chip absolute top-4 right-4 z-10">{{
      t("restart_required")
    }}</span>
    <div class="card-content pt-12">
      <!-- Title -->
      <div class="mb-5">
        <h2 class="text-base font-semibold">{{ t("resolver_title") }}</h2>
        <p class="text-sm text-[hsl(var(--muted-foreground))] mt-1">
          {{ t("resolver_desc") }}
        </p>
      </div>

      <!-- Global config -->
      <section
        class="rounded-lg border border-[hsl(var(--border))] p-4 space-y-4 mb-6">
        <div class="setting-row !py-2">
          <div class="setting-label-group">
            <label class="setting-label">{{ t("rs_enabled") }}</label>
            <p class="setting-desc">{{ t("rs_enabled_desc") }}</p>
          </div>
          <div
            :class="['switch-track', resolver.enabled ? 'on' : 'off']"
            @click="toggleEnabled"
            role="switch"
            :aria-checked="resolver.enabled">
            <span class="switch-thumb"></span>
          </div>
        </div>

        <div
          :class="{
            'pointer-events-none opacity-50 transition-opacity':
              !resolver.enabled,
          }">
          <div class="setting-row !py-2">
            <div class="setting-label-group">
              <label class="setting-label">{{
                t("rs_refresh_interval")
              }}</label>
              <p class="setting-desc">{{ t("rs_refresh_interval_desc") }}</p>
            </div>
            <div class="w-56 max-w-full">
              <CustomSelect
                v-model="resolver.refresh_interval"
                :options="intervalOptions"
                @change="saveResolverConfig" />
            </div>
          </div>

          <div class="setting-row !py-2">
            <div class="setting-label-group">
              <label class="setting-label">{{ t("rs_max_total") }}</label>
              <p class="setting-desc">{{ t("rs_max_total_desc") }}</p>
            </div>
            <div class="w-40 max-w-full">
              <input
                v-model.number="resolver.max_total"
                @blur="saveResolverConfig"
                type="number"
                min="1"
                max="8000"
                class="input" />
            </div>
          </div>
        </div>
      </section>

      <!-- Sub tabs (disabled when domain mapping is off) -->
      <div
        :class="{
          'pointer-events-none opacity-50 transition-opacity':
            !resolver.enabled,
        }">
        <nav
          class="flex gap-1 overflow-x-auto border-b border-[hsl(var(--border))] -mx-1 px-1 mb-5">
          <button
            v-for="s in subTabs"
            :key="s.id"
            type="button"
            :class="['subtab', activeSub === s.id ? 'active' : '']"
            @click="activeSub = s.id">
            {{ t(s.label) }}
          </button>
        </nav>

        <!-- Effective rules -->
        <section v-show="activeSub === 'effective'" class="space-y-4">
          <!-- Stats -->
          <div class="flex items-center gap-3">
            <div class="flex-1">
              <div class="flex items-center justify-between text-xs mb-1">
                <span class="text-[hsl(var(--muted-foreground))]">
                  {{ t("rs_sub_effective") }}:
                  <span class="font-semibold text-[hsl(var(--foreground))]">
                    {{ resolver.total_rules }} / {{ resolver.max_total }}
                  </span>
                </span>
                <span
                  v-if="resolver.total_rules > resolver.max_total"
                  class="text-[hsl(var(--destructive))] font-semibold">
                  {{ t("rs_max_total") }}
                </span>
              </div>
              <div class="progress-track">
                <div
                  class="progress-fill"
                  :class="capExceeded ? 'warning' : 'info'"
                  :style="{ width: capPercent + '%' }"></div>
              </div>
            </div>
            <button
              class="btn btn-outline text-xs shrink-0"
              @click="exportResolverRules">
              <Icon name="save" :size="14" />
              {{ t("rs_export") }}
            </button>
          </div>

          <!-- Rules table -->
          <div
            v-if="resolver.rules.length"
            class="rounded-lg border border-[hsl(var(--border))] overflow-hidden">
            <div class="max-h-[360px] overflow-auto">
              <table class="w-full text-sm">
                <thead
                  class="sticky top-0 bg-[hsl(var(--card))] text-[hsl(var(--muted-foreground))] text-xs">
                  <tr class="border-b border-[hsl(var(--border))]">
                    <th class="text-left font-medium px-3 py-2">
                      {{ t("rs_col_domain") }}
                    </th>
                    <th class="text-left font-medium px-3 py-2">
                      {{ t("rs_col_ip") }}
                    </th>
                    <th class="text-left font-medium px-3 py-2">
                      {{ t("rs_col_source") }}
                    </th>
                  </tr>
                </thead>
                <tbody>
                  <tr
                    v-for="(r, i) in resolver.rules"
                    :key="i"
                    class="border-b border-[hsl(var(--border))] last:border-0">
                    <td class="px-3 py-1.5 font-mono text-xs break-all">
                      {{ r.domain }}
                    </td>
                    <td class="px-3 py-1.5 font-mono text-xs">{{ r.ip }}</td>
                    <td
                      class="px-3 py-1.5 text-xs text-[hsl(var(--muted-foreground))]">
                      {{ r.source }}
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
          </div>
          <div
            v-else
            class="text-sm text-[hsl(var(--muted-foreground))] rounded-lg border border-dashed border-[hsl(var(--border))] p-6 text-center">
            {{ t("rs_empty_rules") }}
          </div>
        </section>

        <!-- Subscriptions -->
        <section v-show="activeSub === 'subscriptions'" class="space-y-4">
          <!-- Add form -->
          <div
            class="rounded-lg border border-[hsl(var(--border))] p-4 space-y-3">
            <label class="setting-label">{{ t("rs_add") }}</label>
            <div class="grid grid-cols-1 sm:grid-cols-[1fr_2fr_auto] gap-2">
              <input
                v-model="newName"
                class="input"
                spellcheck="false"
                :placeholder="t('rs_add_name')" />
              <input
                v-model="newUrl"
                class="input"
                spellcheck="false"
                :placeholder="t('rs_add_url_ph')" />
              <button
                class="btn btn-primary text-xs"
                :disabled="adding || !newName.trim() || !newUrl.trim()"
                @click="onAdd">
                <span
                  v-if="adding"
                  class="inline-block w-3.5 h-3.5 border-2 border-current/30 border-t-current rounded-full animate-spin mr-1.5 align-middle -mt-0.5"></span>
                {{ t("rs_add_btn") }}
              </button>
            </div>
          </div>

          <!-- List -->
          <div v-if="resolver.subscriptions.length" class="space-y-2">
            <div
              v-for="(sub, idx) in resolver.subscriptions"
              :key="idx"
              class="rounded-lg border border-[hsl(var(--border))] overflow-hidden">
              <!-- Header -->
              <div class="flex items-center gap-2 px-3 py-2.5">
                <button
                  class="icon-btn shrink-0"
                  @click="toggleExpand(idx)"
                  :title="t('rs_expand_tip')">
                  <Icon
                    :name="expanded[idx] ? 'chevronDown' : 'chevronRight'"
                    :size="16" />
                </button>
                <div class="min-w-0 flex-1">
                  <div class="flex items-center gap-2">
                    <span class="text-sm font-medium truncate">{{
                      sub.name
                    }}</span>
                    <span class="badge badge-info text-[11px]">
                      {{ sub.rule_count }} {{ t("rs_rule_count") }}
                    </span>
                  </div>
                  <p
                    class="text-xs text-[hsl(var(--muted-foreground))] truncate font-mono">
                    {{ sub.url }}
                  </p>
                </div>
                <span
                  class="text-[11px] text-[hsl(var(--muted-foreground))] shrink-0 hidden sm:block">
                  {{ t("rs_last_refresh") }}: {{ formatTime(sub.last_refresh) }}
                </span>
                <div
                  :class="['switch-track', sub.enabled ? 'on' : 'off']"
                  @click="setSubscriptionEnabled(idx, !sub.enabled)"
                  role="switch"
                  :aria-checked="sub.enabled"
                  :title="t('rs_enable')">
                  <span class="switch-thumb"></span>
                </div>
                <button
                  class="icon-btn shrink-0"
                  @click="startEdit(idx)"
                  :title="t('rs_edit')">
                  <Icon name="edit" :size="16" />
                </button>
                <button
                  class="icon-btn shrink-0"
                  @click="refreshSubscription(idx)"
                  :title="t('rs_refresh')">
                  <Icon name="refresh" :size="16" />
                </button>
                <button
                  class="icon-btn shrink-0 hover:text-[hsl(var(--destructive))]"
                  @click="removeSubscription(idx)"
                  :title="t('rs_remove') || 'Remove'">
                  <Icon name="close" :size="16" />
                </button>
              </div>
              <!-- Edit panel -->
              <div
                v-if="editing[idx]"
                class="border-t border-[hsl(var(--border))] p-3 space-y-2">
                <input
                  v-model="editName"
                  class="input"
                  spellcheck="false"
                  :placeholder="t('rs_add_name')" />
                <input
                  v-model="editUrl"
                  class="input"
                  spellcheck="false"
                  :placeholder="t('rs_add_url_ph')" />
                <div class="flex gap-2 justify-end">
                  <button
                    class="btn btn-outline text-xs"
                    @click="cancelEdit(idx)">
                    {{ t("cancel") }}
                  </button>
                  <button
                    class="btn btn-primary text-xs"
                    :disabled="
                      savingEdit || !editName.trim() || !editUrl.trim()
                    "
                    @click="saveEdit(idx)">
                    <span
                      v-if="savingEdit"
                      class="inline-block w-3.5 h-3.5 border-2 border-current/30 border-t-current rounded-full animate-spin mr-1.5 align-middle -mt-0.5"></span>
                    {{ t("save_changes") }}
                  </button>
                </div>
              </div>
              <!-- Body (rules) -->
              <div
                v-if="expanded[idx]"
                class="border-t border-[hsl(var(--border))]">
                <div
                  v-if="sub.rules && sub.rules.length"
                  class="max-h-[280px] overflow-auto">
                  <table class="w-full text-sm">
                    <tbody>
                      <tr
                        v-for="(r, i) in sub.rules"
                        :key="i"
                        class="border-b border-[hsl(var(--border))] last:border-0">
                        <td
                          class="px-3 py-1.5 pl-10 font-mono text-xs break-all">
                          {{ r.domain }}
                        </td>
                        <td class="px-3 py-1.5 font-mono text-xs">
                          {{ r.ip }}
                        </td>
                      </tr>
                    </tbody>
                  </table>
                </div>
                <div
                  v-else
                  class="px-3 py-3 pl-10 text-xs text-[hsl(var(--muted-foreground))]">
                  {{ t("rs_empty_rules") }}
                </div>
              </div>
            </div>

            <button
              class="btn btn-outline text-xs w-full"
              @click="refreshSubscription(-1)"
              :disabled="!hasEnabledSub">
              <Icon name="refresh" :size="14" />
              {{ t("rs_refresh_all") }}
            </button>
          </div>

          <div
            v-else
            class="text-sm text-[hsl(var(--muted-foreground))] rounded-lg border border-dashed border-[hsl(var(--border))] p-6 text-center">
            {{ t("rs_empty_subs") }}
          </div>
        </section>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from "vue";
import { t } from "../../i18n.js";
import { useStore } from "../../store.js";
import Icon from "../Icon.vue";
import CustomSelect from "../CustomSelect.vue";

const {
  resolver,
  loadResolver,
  saveResolverConfig,
  addSubscription,
  removeSubscription,
  setSubscriptionEnabled,
  refreshSubscription,
  updateSubscription,
  exportResolverRules,
  formatTime,
} = useStore();

const activeSub = ref("effective");
const subTabs = [
  { id: "effective", label: "rs_sub_effective" },
  { id: "subscriptions", label: "rs_sub_subscriptions" },
];

const newName = ref("");
const newUrl = ref("");
const adding = ref(false);
const expanded = reactive({});
const editing = reactive({});
const editName = ref("");
const editUrl = ref("");
const savingEdit = ref(false);

function toggleExpand(idx) {
  expanded[idx] = !expanded[idx];
}

function startEdit(idx) {
  editing[idx] = true;
  editName.value = resolver.subscriptions[idx].name;
  editUrl.value = resolver.subscriptions[idx].url;
}

function cancelEdit(idx) {
  editing[idx] = false;
}

async function saveEdit(idx) {
  if (!editName.value.trim() || !editUrl.value.trim()) return;
  savingEdit.value = true;
  const ok = await updateSubscription(
    idx,
    editName.value.trim(),
    editUrl.value.trim(),
  );
  savingEdit.value = false;
  if (ok) editing[idx] = false;
}

function toggleEnabled() {
  resolver.enabled = !resolver.enabled;
  saveResolverConfig();
}

async function onAdd() {
  if (!newName.value.trim() || !newUrl.value.trim()) return;
  adding.value = true;
  const ok = await addSubscription(newName.value.trim(), newUrl.value.trim());
  adding.value = false;
  if (ok) {
    newName.value = "";
    newUrl.value = "";
  }
}

const intervalOptions = computed(() => [
  { value: 0, label: t("rs_interval_0") },
  { value: 1, label: "1 h" },
  { value: 6, label: "6 h" },
  { value: 12, label: "12 h" },
  { value: 24, label: "24 h" },
  { value: 48, label: "48 h" },
  { value: 168, label: "7 d" },
]);

const capExceeded = computed(() => resolver.total_rules > resolver.max_total);
const capPercent = computed(() => {
  const max = resolver.max_total > 0 ? resolver.max_total : 1;
  return Math.min(100, Math.round((resolver.total_rules / max) * 100));
});

const hasEnabledSub = computed(() =>
  resolver.subscriptions.some((s) => s.enabled),
);

onMounted(() => {
  loadResolver();
});
</script>
