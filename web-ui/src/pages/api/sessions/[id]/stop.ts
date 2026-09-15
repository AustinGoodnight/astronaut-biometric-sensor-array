import type { APIRoute } from "astro";
import { stopSessionRow, getSession } from "../../../../lib/db";

export const prerender = false;

export const POST: APIRoute = async ({ params }) => {
  const id = params.id!;
  const session = getSession(id);
  if (!session) {
    return new Response(JSON.stringify({ error: "Not found" }), { status: 404 });
  }

  const updated = stopSessionRow(id, Date.now());

  return new Response(JSON.stringify(updated), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
};
