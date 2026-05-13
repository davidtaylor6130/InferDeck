import type { FastifyInstance, FastifyError, FastifyReply, FastifyRequest } from "fastify";

function formatError(err: FastifyError): { message: string; type: string; details?: any } {
  const type = err.code ?? (err as any).type ?? "internal_error";

  if (type === "FST_ERR_BAD_INPUT") {
    return { type: "bad_input", message: "Invalid request data" };
  }

  if (type === "FST_ERR_NOT_FOUND") {
    return { type: "not_found", message: "Resource not found" };
  }

  if (type === "FST_ERR_VALIDATION") {
    return { type: "validation_error", message: err.message };
  }

  if (type === "AUTH_INVALID" || type === "AUTH_MISSING") {
    return { type: "authentication_error", message: "Authentication required" };
  }

  const statusCode = (err as any).statusCode ?? (err as any).status ?? 500;
  if (statusCode < 500) {
    return {
      type: (err.message || "Client Error").toLowerCase().replace(/[^\w\s]/g, "").slice(0, 30),
      message: err.message,
    };
  }

  return {
    type: "internal_error",
    message: "An unexpected error occurred",
  };
}

export async function errorHandlerPlugin(app: FastifyInstance): Promise<void> {
  app.setErrorHandler((error: FastifyError, req: FastifyRequest, reply: FastifyReply) => {
    const formatted = formatError(error);

    if ((error as any).statusCode >= 500) {
      app.log.error({
        msg: "Internal server error",
        error: error.message,
        stack: error.stack,
        path: req.url,
        method: req.method,
        requestId: req.headers["x-request-id"] ?? "unknown",
      });
    } else {
      app.log.warn({
        msg: "Request error",
        error: error.message,
        path: req.url,
        method: req.method,
        requestId: req.headers["x-request-id"] ?? "unknown",
      });
    }

    reply
      .status(error.statusCode ?? 500)
      .send({ error: formatted, traceId: req.headers["x-request-id"] as string });
  });

  app.setNotFoundHandler(async (req, reply) => {
    return reply.code(404).send({
      error: {
        type: "not_found",
        message: `Cannot ${req.method} ${req.url}`,
      },
    });
  });
}
