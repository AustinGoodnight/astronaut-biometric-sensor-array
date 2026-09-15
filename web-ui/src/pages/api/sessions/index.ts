import type { APIRoute } from "astro";
import { createSession, listSessions, seedIfEmpty } from "../../../lib/db";

export const prerender = false;

export const GET: APIRoute = async () => {
  seedIfEmpty();
  const sessions = listSessions();
  return new Response(JSON.stringify(sessions), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
};

export const POST: APIRoute = async ({ request }) => {
  seedIfEmpty();
  const body = await request.json();
  const label = typeof body.label === "string" && body.label.trim() ? body.label.trim() : "Recorded Session";
  const user = typeof body.user === "string" && body.user.trim() ? body.user.trim() : "Unassigned";
  const id = `s_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
  const startedAt = Date.now();

  createSession(id, label, user, startedAt);

  return new Response(JSON.stringify({ id, startedAt }), {
    status: 201,
    headers: { "Content-Type": "application/json" },
  });
};
