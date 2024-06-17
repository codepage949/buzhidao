import { config } from "dotenv";
import cryptoJs from "crypto-js";
import pinyin from "pinyin";

config({ export: true });

async function translate(text: string) {
  const uuid = crypto.randomUUID();
  const path = Deno.env.get("PAPAGO_API_URL");
  const timestamp = new Date().valueOf().toString();
  const papagoVersion = Deno.env.get("PAPAGO_VERSION")!;
  const auth = cryptoJs.enc.Base64.stringify(
    cryptoJs.HmacMD5(
      `${uuid}\n${path}\n${timestamp}`,
      papagoVersion,
    ),
  );
  const usp = new URLSearchParams();

  usp.set("deviceId", uuid);
  usp.set("locale", "en");
  usp.set("dict", "true");
  usp.set("dictDisplay", "30");
  usp.set("honorific", "false");
  usp.set("instant", "false");
  usp.set("paging", "false");
  usp.set("source", (Deno.env.get("SOURCE") === "en") ? "en" : "zh-CN");
  usp.set("target", "ko");
  usp.set("text", text);

  const resp = await fetch(Deno.env.get("PAPAGO_API_URL")!, {
    method: "post",
    headers: new Headers({
      "authorization": `PPG ${uuid}:${auth}`,
      "timestamp": timestamp,
    }),
    body: usp,
  });

  console.log(Deno.env.get("PAPAGO_API_URL")!, resp.status);

  return await resp.json();
}

async function makeMessage(txts: string[]) {
  const joinedTxt = txts.join("\n\n");
  let pnins: string[];

  if (Deno.env.get("SOURCE")! === "ch") {
    pnins = pinyin(joinedTxt).flat().join(" ").split("\n\n");
  }
  
  const translateResult = await translate(joinedTxt);

  console.log("translateResult", translateResult);

  const translatedTxts = translateResult.translatedText.split("\n\n");
  let dict = "";

  for (const item of (translateResult.dict?.items) ?? []) {
    dict += `## ${item.entry.replace(/<.*?>/g, "")} ${(Deno.env.get("SOURCE")! === "ch") ? pinyin(item.entry.replace(/<.*?>/g, "")).flat().join(" ") : ""}\n`;

    for (const pos of item.pos) {
        for (const meaning of pos.meanings) {
            dict += `1. ${meaning.meaning.replace(/<.*?>/g, "")}\n`
        }
    }

    dict += "\n";
  }

  const output = [];

  for (let i = 0; i < txts.length; i++) {
    if (Deno.env.get("SOURCE")! === "ch") {
      output.push(
        [txts[i], pnins![i].trim(), translatedTxts[i]].join("\n"),
      );
    } else {
      output.push(
        [txts[i], translatedTxts[i]].join("\n"),
      );
    }
  }

  return output.join("\n\n") + "\n\n" + dict;
}

async function* infinity() {
  while (true) {
    yield;

    await new Promise((ok) => {
      setTimeout(ok, 0);
    });
  }
}

function isSourceLanguage(text: string) {
  return (Deno.env.get("SOURCE")! === "en") ? (/[a-zA-Z]/g).test(text): (/[\u4e00-\u9fa5]/g).test(text);
}

async function pollTgMessage() {
  let offset = 0;

  for await (const _ of infinity()) {
    try {
      const resp = await fetch(
        `https://api.telegram.org/bot${Deno.env.get(
          "BOT_TOKEN",
        )!}/getUpdates?offset=${offset}`,
        {
          method: "get",
        },
      );

      console.log(`https://api.telegram.org/bot${Deno.env.get(
        "BOT_TOKEN",
      )!}/getUpdates?offset=${offset}`, resp.status);

      const update = await resp.json();

      for (const result of update.result) {
        offset = result.update_id + 1;

        if (Deno.env.get("CHAT_ID")!) {
          if ("photo" in result.message) {
            const photo = result.message.photo.reverse()[0];

            let resp = await fetch(
              `https://api.telegram.org/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/getFile`,
              {
                method: "post",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({
                  file_id: photo.file_id,
                }),
              },
            );
  
            console.log(`https://api.telegram.org/bot${Deno.env.get("BOT_TOKEN")!}/getFile`, resp.status);

            const file = (await resp.json())["result"];

            resp = await fetch(
              `https://api.telegram.org/file/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/${file.file_path}`,
              {
                method: "get",
              },
            );

            const fd = new FormData();

            fd.set(
              "file",
              new File([await resp.bytes()], "output.jpg"),
            );

            resp = await fetch(
              `${Deno.env.get("INFER_API_URL")!}/infer/${Deno.env.get("SOURCE")!}`,
              {
                method: "post",
                body: fd,
              },
            );

            console.log(`${Deno.env.get("INFER_API_URL")!}/infer/${Deno.env.get("SOURCE")!}`, resp.status);

            const detectionMap = new Map();
            const detections = await resp.json();

            for (const detection of detections) {
              const [leftUpper, , , leftBottom] = detection[0];
              const [txt] = detection[1];

              if (isSourceLanguage(txt)) {
                let near = null;

                for (const [candidateDetection] of detectionMap) {
                  const [x1, y1] = candidateDetection;
                  const [x2, y2] = leftUpper;

                  if ((x2 - x1) ** 2 <= parseInt(Deno.env.get("X_DELTA")!) && (y2 - y1) ** 2 <= parseInt(Deno.env.get("Y_DELTA")!)) {
                    near = candidateDetection;

                    break;
                  }
                }

                if (near) {
                  const oldTxt = detectionMap.get(near);

                  detectionMap.set(leftBottom, oldTxt + txt.trim());
                  detectionMap.delete(near);
                } else {
                  detectionMap.set(leftBottom, txt.trim());
                }
              }
            }

            let message = "no detections.";

            if (detectionMap.size > 0) {
              const txts = [];

              for (const [, txt] of detectionMap) {
                txts.push(txt);
              }

              message = await makeMessage(txts);
            }

            resp = await fetch(
              `https://api.telegram.org/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/sendMessage`,
              {
                method: "post",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({
                  chat_id: Deno.env.get("CHAT_ID")!,
                  text: message,
                }),
              },
            );

            console.log(`https://api.telegram.org/bot${Deno.env.get(
              "BOT_TOKEN",
            )!}/sendMessage`, resp.status, await resp.json());
          }
        } else {
          const resp = await fetch(
            `https://api.telegram.org/bot${Deno.env.get(
              "BOT_TOKEN",
            )!}/sendMessage`,
            {
              method: "post",
              headers: { "content-type": "application/json" },
              body: JSON.stringify({
                chat_id: result.message.chat.id,
                text: result.message.chat.id,
              }),
            },
          );

          console.log(`https://api.telegram.org/bot${Deno.env.get(
            "BOT_TOKEN",
          )!}/sendMessage`, resp.status, await resp.json());
        }
      }

      await new Promise((ok) => {
        setTimeout(ok, 300);
      });
    } catch (e) {
      console.error(e);
    }
  }
}

(function main() {
  pollTgMessage();
})();
