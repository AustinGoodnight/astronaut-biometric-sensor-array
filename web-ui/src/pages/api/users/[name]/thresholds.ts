import type { APIRoute } from "astro";
import { getUserThresholds } from "../../../../lib/db";

export const prerender = false;

export const GET: APIRoute = async ({ params }) => {
  const name = decodeURIComponent(params.name!);
  const thresholds = getUserThresholds(name);
  return new Response(JSON.stringify({ name, thresholds }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
};
