import type { Env } from "./sources";

const STATE_GENERATIONS = new WeakMap<object, number>();

function keyFor(env: Env): object {
  return env.DB as unknown as object;
}

export function stateGeneration(env: Env): number {
  return STATE_GENERATIONS.get(keyFor(env)) ?? 0;
}

export function markStateChanged(env: Env): number {
  const key = keyFor(env);
  const next = (STATE_GENERATIONS.get(key) ?? 0) + 1;
  STATE_GENERATIONS.set(key, next);
  return next;
}

export function resetStateGeneration(env: Env): void {
  STATE_GENERATIONS.delete(keyFor(env));
}
