import type { APIRoute } from "astro";
import { getSession, getReadings } from "../../../../lib/db";

export const prerender = false;

export const GET: APIRoute = async ({ params }) => {
  const id = params.id!;
  const session = getSession(id);
  if (!session) {
    return new Response(JSON.stringify({ error: "Not found" }), { status: 404 });
  }
  const readings = getReadings(id);
  return new Response(JSON.stringify({ session, readings }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
};
