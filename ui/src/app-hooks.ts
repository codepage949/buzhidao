import { useEffect, type DependencyList } from "react";
import type { UnlistenFn } from "@tauri-apps/api/event";

export function useListenerCleanup(
  register: () => Array<Promise<UnlistenFn>>,
  deps: DependencyList,
) {
  useEffect(() => {
    const unlistens = register();
    return () => {
      unlistens.forEach((promise) => promise.then((unlisten) => unlisten()));
    };
  }, deps);
}

export function useWindowKeydown(
  handler: (event: KeyboardEvent) => void,
  deps: DependencyList,
) {
  useEffect(() => {
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, deps);
}
