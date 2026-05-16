import { readdirSync, statSync } from "node:fs";
import { join, extname, relative } from "node:path";
import type { LlamaModelInfo } from "./types.js";

export function scanGgufFiles(
  directory: string,
  baseDir?: string
): LlamaModelInfo[] {
  const resolvedDir = join(directory);
  const root = baseDir ?? resolvedDir;
  const results: LlamaModelInfo[] = [];

  try {
    const entries = readdirSync(resolvedDir, { withFileTypes: true });
    for (const entry of entries) {
      const fullPath = join(resolvedDir, entry.name);
      if (entry.isDirectory()) {
        results.push(...scanGgufFiles(fullPath, root));
      } else if (entry.isFile() && extname(entry.name).toLowerCase() === ".gguf") {
        try {
          const stats = statSync(fullPath);
          results.push({
            name: relative(root, fullPath),
            path: fullPath,
            size: stats.size,
            modified_at: stats.mtime.toISOString(),
            format: "gguf",
          });
        } catch {
          // skip unreadable files
        }
      }
    }
  } catch {
    // directory doesn't exist or not readable
  }

  return results;
}
