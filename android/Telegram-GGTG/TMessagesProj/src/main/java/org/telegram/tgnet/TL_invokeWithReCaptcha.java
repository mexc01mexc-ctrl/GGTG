package org.telegram.tgnet;

// ggtg: MTProto wrapper for reCAPTCHA Enterprise verification: invokeWithReCaptcha#adbb0f94 {X:Type} token:string query:!X = X;
public class TL_invokeWithReCaptcha extends TLObject {
    public static final int constructor = 0xadbb0f94;

    public String token;
    public TLObject query;

    public TL_invokeWithReCaptcha() {
    }

    public TL_invokeWithReCaptcha(String token, TLObject query) {
        this.token = token;
        this.query = query;
    }

    @Override
    public TLObject deserializeResponse(InputSerializedData stream, int constructor, boolean exception) {
        if (query != null) {
            return query.deserializeResponse(stream, constructor, exception);
        }
        return null;
    }

    @Override
    public void serializeToStream(OutputSerializedData stream) {
        stream.writeInt32(constructor);
        stream.writeString(token != null ? token : "");
        if (query != null) {
            query.serializeToStream(stream);
        }
    }

    @Override
    public void freeResources() {
        if (query != null) {
            query.freeResources();
        }
    }
}
