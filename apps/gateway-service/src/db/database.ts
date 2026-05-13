import initSqlJs, { Database as SqlJsDatabase } from "sql.js";
import { readFileSync, writeFileSync, readdirSync, existsSync, mkdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import pino from "pino";

const LOG = pino({ name: "db" });
const moduleDir = typeof import.meta.url === "string" ? dirname(fileURLToPath(import.meta.url)) : process.cwd();

function findWasmFile(): string {
  const possiblePaths = [
    (process as any).resourcesPath ? join((process as any).resourcesPath, "sql-wasm.wasm") : "",
    join(process.cwd(), "sql-wasm.wasm"),
    join(dirname(process.execPath), "sql-wasm.wasm"),
    join(moduleDir, "sql-wasm.wasm"),
    join(moduleDir, "..", "sql-wasm.wasm"),
  ];

  for (const p of possiblePaths) {
    try {
      if (existsSync(p)) return p;
    } catch {}
  }

  // Fallback: let sql.js try to fetch from CDN
  return "";
}

export class DatabaseManager {
  private db: SqlJsDatabase | null = null;
  private dbPath: string;
  private migrationsDir: string;
  private initialized = false;

  constructor(dbPath: string, migrationsDir: string) {
    this.dbPath = dbPath;
    this.migrationsDir = migrationsDir;
  }

  async init(): Promise<void> {
    if (this.initialized) return;

    const wasmPath = findWasmFile();
    let SQL;

    if (wasmPath && existsSync(wasmPath)) {
      const wasmBinary = readFileSync(wasmPath);
      SQL = await initSqlJs({ wasmBinary });
    } else {
      SQL = await initSqlJs();
    }

    let data: Uint8Array | undefined;
    if (existsSync(this.dbPath)) {
      try {
        data = readFileSync(this.dbPath);
      } catch (e) {
        LOG.warn("Could not read existing DB, creating new one");
      }
    }

    this.db = data ? new SQL.Database(data) : new SQL.Database();
    this.runMigrations();
    this.initialized = true;
    LOG.info(`Database initialized at ${this.dbPath}`);
  }

  private runMigrations(): void {
    if (!this.db) throw new Error("DB not initialized");

    const migrationFiles = readdirSync(this.migrationsDir)
      .filter((f) => f.endsWith(".sql"))
      .sort();

    try {
      const result = this.db.exec("SELECT name FROM schema_migrations");
      if (result.length > 0) {
        result[0].values.forEach((row: unknown[]) => this.appliedMigrations.add(row[0] as string));
      }
    } catch {
      // schema_migrations may not exist yet for the first run
    }

    for (const file of migrationFiles) {
      if (this.appliedMigrations.has(file)) continue;
      LOG.info(`Running migration: ${file}`);
      const sql = readFileSync(join(this.migrationsDir, file), "utf-8");
      this.db.exec(sql);
      LOG.info(`Migration applied: ${file}`);
    }

    this.save();
  }

  private appliedMigrations: Set<string> = new Set();

  get client(): SqlJsDatabase {
    if (!this.db) throw new Error("DB not initialized");
    return this.db;
  }

  prepare(sql: string): any {
    if (!this.db) throw new Error("DB not initialized");
    return this.db.prepare(sql);
  }

  exec(sql: string): void {
    if (!this.db) throw new Error("DB not initialized");
    this.db.exec(sql);
    this.save();
  }

  save(): void {
    if (!this.db) return;
    const data = this.db.export();
    const buffer = Buffer.from(data);
    const dir = dirname(this.dbPath);
    if (!existsSync(dir)) {
      mkdirSync(dir, { recursive: true });
    }
    writeFileSync(this.dbPath, buffer);
  }

  close(): void {
    if (this.db) {
      this.save();
      this.db.close();
    }
  }
}
