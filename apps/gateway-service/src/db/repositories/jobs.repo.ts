import type { DatabaseManager } from "../database";

export interface JobRepo {
  insert(job: {
    id: string;
    type: string;
    priority: number;
    resourceClass: string;
    clientName: string | null;
    requestPath: string | null;
    requestMethod: string | null;
    payload: Record<string, unknown>;
    maxRetries: number;
    idempotencyKey: string | null;
  }): void;
  getById(id: string): any;
  updateStatus(id: string, status: string, extraFields?: Record<string, unknown>): void;
  list(status: string | null, limit: number, offset: number): { rows: any[]; total: number };
  cancel(id: string): void;
  retry(id: string): void;
  reprioritize(id: string, priority: number): void;
  delete(id: string): void;
  insertEvent(jobId: string, eventType: string, message: string, data: Record<string, unknown>): void;
  getEvents(jobId: string): any[];
  getResult(id: string): any;
  updateResult(id: string, result: Record<string, unknown>): void;
  updateError(id: string, error: Record<string, unknown>): void;
  getQueued(limit: number): any[];
  cleanupOld(status: string, days: number): number;
}

export function createJobRepository(db: DatabaseManager): JobRepo {
  const client = db.client;

  function runQuery(sql: string, params: any[] = []): void {
    client.run(sql, params);
    db.save();
  }

  function getOne(sql: string, params: any[] = []): any {
    const stmt = client.prepare(sql);
    stmt.bind(params);
    if (stmt.step()) {
      const row = stmt.getAsObject();
      stmt.free();
      return row;
    }
    stmt.free();
    return null;
  }

  function getAll(sql: string, params: any[] = []): any[] {
    const results: any[] = [];
    const stmt = client.prepare(sql);
    stmt.bind(params);
    while (stmt.step()) {
      results.push(stmt.getAsObject());
    }
    stmt.free();
    return results;
  }

  return {
    insert(job) {
      runQuery(
        `INSERT INTO jobs (id, type, priority, resource_class, client_name, request_path, request_method, payload_json, max_retries, idempotency_key, status, created_at, updated_at)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'queued', datetime('now'), datetime('now'))`,
        [job.id, job.type, job.priority, job.resourceClass, job.clientName, job.requestPath, job.requestMethod, JSON.stringify(job.payload), job.maxRetries, job.idempotencyKey]
      );
    },

    getById(id) {
      return getOne(`SELECT * FROM jobs WHERE id = ?`, [id]);
    },

    updateStatus(id, status, extraFields) {
      if (extraFields) {
        const sets = Object.keys(extraFields).map(k => `${k} = ?`).join(', ');
        const vals = Object.values(extraFields);
        runQuery(`UPDATE jobs SET status = ?, ${sets}, updated_at = datetime('now') WHERE id = ?`, [status, ...vals, id]);
      } else {
        runQuery(`UPDATE jobs SET status = ?, updated_at = datetime('now') WHERE id = ?`, [status, id]);
      }
    },

    list(status, limit, offset) {
      let sql = status 
        ? `SELECT * FROM jobs WHERE status = ? ORDER BY priority DESC, created_at ASC LIMIT ? OFFSET ?`
        : `SELECT * FROM jobs ORDER BY priority DESC, created_at ASC LIMIT ? OFFSET ?`;
      
      const params = status ? [status, limit, offset] : [limit, offset];
      const rows = getAll(sql, params);
      
      const countSql = status 
        ? `SELECT COUNT(*) as total FROM jobs WHERE status = ?`
        : `SELECT COUNT(*) as total FROM jobs`;
      const countResult = status ? getOne(countSql, [status]) : getOne(countSql, []);
      const total = countResult?.total ?? 0;

      return { rows, total };
    },

    cancel(id) {
      runQuery(`UPDATE jobs SET status = 'cancelled', updated_at = datetime('now') WHERE id = ?`, [id]);
    },

    retry(id) {
      runQuery(`UPDATE jobs SET status = 'queued', retry_count = retry_count + 1, updated_at = datetime('now') WHERE id = ?`, [id]);
    },

    reprioritize(id, priority) {
      runQuery(`UPDATE jobs SET priority = ?, updated_at = datetime('now') WHERE id = ?`, [priority, id]);
    },

    delete(id) {
      runQuery(`DELETE FROM jobs WHERE id = ?`, [id]);
    },

    insertEvent(jobId, eventType, message, data) {
      runQuery(
        `INSERT INTO job_events (job_id, event_type, message, data_json, created_at) VALUES (?, ?, ?, ?, datetime('now'))`,
        [jobId, eventType, message, JSON.stringify(data)]
      );
    },

    getEvents(jobId) {
      return getAll(`SELECT * FROM job_events WHERE job_id = ? ORDER BY id DESC`, [jobId]);
    },

    getResult(id) {
      const row = getOne(`SELECT result_json FROM jobs WHERE id = ?`, [id]);
      return row?.result_json ? JSON.parse(row.result_json) : null;
    },

    updateResult(id, result) {
      runQuery(`UPDATE jobs SET result_json = ?, status = 'succeeded', updated_at = datetime('now') WHERE id = ?`, [JSON.stringify(result), id]);
    },

    updateError(id, error) {
      runQuery(`UPDATE jobs SET error_json = ?, status = 'failed', updated_at = datetime('now') WHERE id = ?`, [JSON.stringify(error), id]);
    },

    getQueued(limit) {
      return getAll(`SELECT * FROM jobs WHERE status = 'queued' ORDER BY priority DESC, created_at ASC LIMIT ?`, [limit]);
    },

    cleanupOld(status, days) {
      runQuery(`DELETE FROM jobs WHERE status = ? AND updated_at < datetime('now', '-' || ? || ' days')`, [status, days]);
      return 0;
    },
  };
}