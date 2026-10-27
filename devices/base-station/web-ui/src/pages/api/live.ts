import type { APIRoute } from "astro";
import { getLiveSnapshot } from "../../lib/sensorLoop";
import { getActiveSessions } from "../../lib/db";

export const prerender = false;

export const GET: APIRoute = async () => {
  const { current, history } = getLiveSnapshot();
  const isLive = getActiveSessions().length > 0;
  return new Response(JSON.stringify({ isLive, current, history }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
};
