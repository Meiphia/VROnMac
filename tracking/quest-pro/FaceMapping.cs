// Adapted from korejan/VRCFT-ALXR-Modules ModuleUtils.cs (MIT).
// Original license retained in ALXR-LICENSE. Multipliers are neutral (1.0).
using VRCFaceTracking.Core.Params.Data;
using VRCFaceTracking.Core.Params.Expressions;
namespace VRConMac.Tracking;
public static class FaceMapping {
        public static void UpdateEyeExpressionsFBV2(ref UnifiedExpressionShape[] unifiedExpressions, ReadOnlySpan<float> expressions)
        {
            #region V2 Eye Expressions Set

            unifiedExpressions[(int)UnifiedExpressions.EyeWideLeft].Weight = Math.Min(1.0f, expressions[59] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.EyeWideRight].Weight = Math.Min(1.0f, expressions[60] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.EyeSquintLeft].Weight = Math.Min(1.0f, expressions[28] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.EyeSquintRight].Weight = Math.Min(1.0f, expressions[29] * 1f);

            #endregion

            #region V2 Brow Expressions Set

            unifiedExpressions[(int)UnifiedExpressions.BrowInnerUpLeft].Weight = Math.Min(1.0f, expressions[22] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.BrowInnerUpRight].Weight = Math.Min(1.0f, expressions[23] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.BrowOuterUpLeft].Weight = Math.Min(1.0f, expressions[57] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.BrowOuterUpRight].Weight = Math.Min(1.0f, expressions[58] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.BrowLowererLeft].Weight = Math.Min(1.0f, expressions[0] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.BrowPinchLeft].Weight = Math.Min(1.0f, expressions[0] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.BrowLowererRight].Weight = Math.Min(1.0f, expressions[1] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.BrowPinchRight].Weight = Math.Min(1.0f, expressions[1] * 1f);

            #endregion
        }
        public static void UpdateMouthExpressionsFBV2(ref UnifiedExpressionShape[] unifiedExpressions, ReadOnlySpan<float> expressions)
        {
            #region V2 Jaw Expression Set                        
            unifiedExpressions[(int)UnifiedExpressions.JawOpen].Weight = Math.Min(1.0f, expressions[24] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.JawLeft].Weight = Math.Min(1.0f, expressions[25] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.JawRight].Weight = Math.Min(1.0f, expressions[26] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.JawForward].Weight = Math.Min(1.0f, expressions[27] * 1f);
            #endregion

            #region V2 Mouth Expression Set   
            unifiedExpressions[(int)UnifiedExpressions.MouthClosed].Weight = expressions[50];

            unifiedExpressions[(int)UnifiedExpressions.MouthUpperLeft].Weight = Math.Min(1.0f, expressions[53] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthLowerLeft].Weight = Math.Min(1.0f, expressions[53] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthUpperRight].Weight = Math.Min(1.0f, expressions[54] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthLowerRight].Weight = Math.Min(1.0f, expressions[54] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.MouthCornerPullLeft].Weight = Math.Min(1.0f, expressions[32] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthCornerSlantLeft].Weight = Math.Min(1.0f, expressions[32] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthCornerPullRight].Weight = Math.Min(1.0f, expressions[33] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthCornerSlantRight].Weight = Math.Min(1.0f, expressions[33] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthFrownLeft].Weight = Math.Min(1.0f, expressions[30] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthFrownRight].Weight = Math.Min(1.0f, expressions[31] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.MouthLowerDownLeft].Weight    = Math.Min(1.0f, expressions[51] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthLowerDownRight].Weight   = Math.Min(1.0f, expressions[52] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthUpperUpLeft].Weight      = Math.Min(1.0f, 1f * Math.Max(expressions[61], expressions[55])); // Workaround for wierd tracking quirk.
            unifiedExpressions[(int)UnifiedExpressions.MouthUpperDeepenLeft].Weight  = Math.Min(1.0f, 1f * Math.Max(expressions[61], expressions[55])); // Workaround for wierd tracking quirk.
            unifiedExpressions[(int)UnifiedExpressions.MouthUpperUpRight].Weight     = Math.Min(1.0f, 1f * Math.Max(expressions[62], expressions[56])); // Workaround for wierd tracking quirk.
            unifiedExpressions[(int)UnifiedExpressions.MouthUpperDeepenRight].Weight = Math.Min(1.0f, 1f * Math.Max(expressions[62], expressions[56])); // Workaround for wierd tracking quirk.

            unifiedExpressions[(int)UnifiedExpressions.MouthRaiserUpper].Weight = Math.Min(1.0f, expressions[9] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthRaiserLower].Weight = Math.Min(1.0f, expressions[8] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.MouthDimpleLeft].Weight = Math.Min(1.0f, expressions[10] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthDimpleRight].Weight = Math.Min(1.0f, expressions[11] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.MouthTightenerLeft].Weight = Math.Min(1.0f, expressions[48] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthTightenerRight].Weight = Math.Min(1.0f, expressions[49] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.MouthPressLeft].Weight = Math.Min(1.0f, expressions[38] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthPressRight].Weight = Math.Min(1.0f, expressions[39] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.MouthStretchLeft].Weight = Math.Min(1.0f, expressions[42] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.MouthStretchRight].Weight = Math.Min(1.0f, expressions[43] * 1f);
            #endregion

            #region V2 Lip Expression Set   
            unifiedExpressions[(int)UnifiedExpressions.LipPuckerUpperRight].Weight = Math.Min(1.0f, expressions[41] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.LipPuckerLowerRight].Weight = Math.Min(1.0f, expressions[41] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.LipPuckerUpperLeft].Weight = Math.Min(1.0f, expressions[40] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.LipPuckerLowerLeft].Weight = Math.Min(1.0f, expressions[40] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.LipFunnelUpperLeft].Weight = Math.Min(1.0f, expressions[35] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.LipFunnelUpperRight].Weight = Math.Min(1.0f, expressions[37] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.LipFunnelLowerLeft].Weight = Math.Min(1.0f, expressions[34] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.LipFunnelLowerRight].Weight = Math.Min(1.0f, expressions[36] * 1f);

            unifiedExpressions[(int)UnifiedExpressions.LipSuckUpperLeft].Weight = Math.Min(1.0f, 1f * Math.Min(1f - (float)Math.Pow(expressions[61], 1f / 6f), expressions[45]));
            unifiedExpressions[(int)UnifiedExpressions.LipSuckUpperRight].Weight = Math.Min(1.0f, 1f * Math.Min(1f - (float)Math.Pow(expressions[62], 1f / 6f), expressions[47]));
            unifiedExpressions[(int)UnifiedExpressions.LipSuckLowerLeft].Weight = Math.Min(1.0f, 1f * expressions[44]);
            unifiedExpressions[(int)UnifiedExpressions.LipSuckLowerRight].Weight = Math.Min(1.0f, 1f * expressions[46]);
            #endregion

            #region V2 Cheek Expression Set   
            unifiedExpressions[(int)UnifiedExpressions.CheekPuffLeft].Weight = Math.Min(1.0f, expressions[2] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.CheekPuffRight].Weight = Math.Min(1.0f, expressions[3] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.CheekSuckLeft].Weight = Math.Min(1.0f, expressions[6] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.CheekSuckRight].Weight = Math.Min(1.0f, expressions[7] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.CheekSquintLeft].Weight = Math.Min(1.0f, expressions[4] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.CheekSquintRight].Weight = Math.Min(1.0f, expressions[5] * 1f);
            #endregion

            #region V2 Nose Expression Set             
            unifiedExpressions[(int)UnifiedExpressions.NoseSneerLeft].Weight = Math.Min(1.0f, expressions[55] * 1f);
            unifiedExpressions[(int)UnifiedExpressions.NoseSneerRight].Weight = Math.Min(1.0f, expressions[56] * 1f);
            #endregion

            #region V2 Tongue Expression Set

                unifiedExpressions[(int)UnifiedExpressions.TongueOut].Weight = expressions[68];
                unifiedExpressions[(int)UnifiedExpressions.TongueCurlUp].Weight = expressions[64];
                // no current mappings
                // unifiedExpressions[(int)UnifiedExpressions.TongueOut].Weight = expressions[63];
                // unifiedExpressions[(int)UnifiedExpressions.TongueOut].Weight = expressions[65];
                // unifiedExpressions[(int)UnifiedExpressions.TongueOut].Weight = expressions[66];
                // unifiedExpressions[(int)UnifiedExpressions.TongueOut].Weight = expressions[67];
                // unifiedExpressions[(int)UnifiedExpressions.TongueOut].Weight = expressions[69];
            
            #endregion
        }
}
