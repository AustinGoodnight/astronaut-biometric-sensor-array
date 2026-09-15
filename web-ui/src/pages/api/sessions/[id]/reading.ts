import type { APIRoute } from "astro";
import { addReading, getSession, getUserThresholds } from "../../../../lib/db";
import { METRICS } from "../../../../lib/metrics";

export const prerender = false;

export const POST: APIRoute = async ({ params, request }) => {
  const id = params.id!;
  const session = getSession(id);
  if (!session) {
    return new Response(JSON.stringify({ error: "Not found" }), { status: 404 });
  }

  const body = await request.json();
  const values: Record<string, number> = {
    hr: Number(body.hr),
    perspiration: Number(body.perspiration),
    skinTemp: Number(body.skinTemp),
    coreTemp: Number(body.coreTemp),
    respiration: Number(body.respiration),
  };

  const thresholds = getUserThresholds(session.user);
  const elevated = METRICS.some((m) => {
    const v = values[m.key];
    const [low, high] = thresholds[m.key];
    return Number.isFinite(v) && (v < low || v > high);
  });

  addReading(id, Date.now(), values, elevated);

  return new Response(JSON.stringify({ ok: true }), {
    status: 201,
    headers: { "Content-Type": "application/json" },
  });
};
