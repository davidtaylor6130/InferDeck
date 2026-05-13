import type { FastifyInstance } from "fastify";
import { createJobRepository } from "../../db/repositories/jobs.repo";

export function registerLogsRoutes(app: FastifyInstance): void {
  app.get("/logs", async (req, reply) => {
    const ctx = (app as any).ctx();
    const query = req.query as { tab?: string; level?: string; service?: string; jobId?: string; search?: string; limit?: string };
    if (query.tab === "job-events") {
      const repo = createJobRepository(ctx.db);
      const limit = Math.min(Number(query.limit ?? 500), 1000);
      const rows = ctx.db.client.exec(
        `SELECT job_id, event_type, message, data_json, created_at FROM job_events ORDER BY created_at DESC LIMIT ${limit}`
      );
      const logs = rows[0]?.values.map((row: unknown[]) => ({
        timestamp: row[4],
        level: "info",
        service: "job-events",
        jobId: row[0],
        message: `${row[1]} ${row[2] ?? ""}`.trim(),
        data: row[3] ? JSON.parse(String(row[3])) : null,
      })) ?? [];
      return reply.send({ logs });
    }
    const tab = query.tab === "service-errors" ? "service-errors" : "gateway";
    return reply.send({
      logs: ctx.logs.list({
        tab,
        level: query.level,
        service: query.service,
        jobId: query.jobId,
        search: query.search,
        limit: query.limit ? Number(query.limit) : 500,
      }),
    });
  });
}
