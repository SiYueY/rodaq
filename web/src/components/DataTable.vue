<script setup lang="ts" generic="T">
export interface TableColumn {
  key: string;
  label: string;
  numeric?: boolean;
}

defineProps<{
  columns: readonly TableColumn[];
  rows: readonly T[];
  rowKey: (row: T, index: number) => string;
  emptyText?: string;
  caption?: string;
}>();
</script>

<template>
  <div class="table-wrap">
    <table class="data-table">
      <caption v-if="caption" class="sr-only">{{ caption }}</caption>
      <thead>
        <tr>
          <th
            v-for="col in columns"
            :key="col.key"
            scope="col"
            :class="{ num: col.numeric }"
          >
            {{ col.label }}
          </th>
        </tr>
      </thead>
      <tbody>
        <tr v-if="rows.length === 0">
          <td :colspan="columns.length" class="muted">{{ emptyText ?? 'No records' }}</td>
        </tr>
        <template v-else>
          <tr v-for="(row, idx) in rows" :key="rowKey(row, idx)">
            <slot name="row" :row="row" :index="idx" />
          </tr>
        </template>
      </tbody>
    </table>
  </div>
</template>
