import assert from "node:assert/strict";
export async function testShellRecovery({ page, run, check, fresh, hold, release, count, stage }) {
  await check(
    "Pre-reboot capability work is cancelled or discarded and fresh features replace it",
    async () => {
      await fresh("dashboard");
      await page.wait(
        "!document.querySelector('#refresh').disabled && !document.querySelector('#retry-features').disabled"
      );
      const before = await count("GET /api/v1/capabilities"),
        key = "GET /api/v1/status";
      await hold(key);
      await run(() => document.querySelector("#refresh").click());
      await page.wait("!!t.pending['GET /api/v1/status']");
      await run(() => {
        document.querySelector("#retry-features").click();
        t.data["/api/v1/status"].firmware.version = "changed-build";
        t.data["/api/v1/capabilities"].features.custom_pages = false;
      });
      assert.equal(await count("GET /api/v1/capabilities"), before);
      await release(key);
      await page.wait(
        "document.querySelector('[data-feature=custom_pages]').hidden && !document.querySelector('#retry-features').disabled"
      );
      const reads = await count("GET /api/v1/capabilities");
      assert(
        reads >= before + 1 && reads <= before + 2,
        "Expected one fresh read plus at most one old admitted read"
      );
    }
  );
  await check("Destroyed DraftForm cannot read after write or restart itself", async () => {
    const result = await page.evaluate(
      `(async()=>{const {DraftForm}=await import('/__test/core.js');const f=document.createElement('form');f.innerHTML='<input name="value"><button type="submit">Save</button>';let reads=0,release;const d=new DraftForm(f,{read:async()=>{reads++;return {value:'a'};},write:()=>new Promise(r=>release=r)});await d.load();f.elements.value.value='b';const save=d.save();d.destroy();release({});await save;await d.load();await d.save();d.reset();return {reads,baseline:d.baseline};})()`
    );
    assert.deepEqual(result, { reads: 1, baseline: null });
  });
}
