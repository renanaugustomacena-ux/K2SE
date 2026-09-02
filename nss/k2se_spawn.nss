// K2SE spawner script. K2SE runs it (CVirtualMachine::RunScript) on the player a
// moment after every area setup and then every few seconds of gameplay. It asks
// K2SE for the spawn entries of the current module, finds the ones already in
// the area (every object it creates is stamped with LocalBoolean 150 and
// LocalNumber 25 = entry index, both saved with the area) and creates the
// missing ones with the engine's own CreateObject.
//
// Compiled against nwscript_k2se.nss (routines 898..907), deployed as
// override/k2se_spawn.ncs by tools/deploy_movement.py.

int K2SE_SPAWN_FLAG = 150;   // LocalBoolean index: "created by K2SE"
int K2SE_SPAWN_INDEX = 25;   // LocalNumber index: entry number (1..255)

void main()
{
    object oPC = GetFirstPC();
    if (!GetIsObjectValid(oPC)) return;
    object oArea = GetArea(oPC);
    if (!GetIsObjectValid(oArea)) return;

    int nCount = K2SE_SpawnBegin(GetModuleName(), GetTag(oArea));
    if (nCount <= 0) return;

    int nFilter = OBJECT_TYPE_PLACEABLE | OBJECT_TYPE_CREATURE;
    object o = GetFirstObjectInArea(oArea, nFilter);
    while (GetIsObjectValid(o))
    {
        if (GetLocalBoolean(o, K2SE_SPAWN_FLAG))
        {
            K2SE_SpawnMarkPresent(GetLocalNumber(o, K2SE_SPAWN_INDEX));
        }
        o = GetNextObjectInArea(oArea, nFilter);
    }

    int i;
    for (i = 1; i <= nCount; i++)
    {
        if (K2SE_SpawnNeeded(i))
        {
            vector vPos = Vector(K2SE_SpawnX(i), K2SE_SpawnY(i), K2SE_SpawnZ(i));
            location lLoc = Location(vPos, K2SE_SpawnFacing(i));
            object oNew = CreateObject(K2SE_SpawnType(i), K2SE_SpawnTemplate(i), lLoc, FALSE);
            if (GetIsObjectValid(oNew))
            {
                SetLocalBoolean(oNew, K2SE_SPAWN_FLAG, TRUE);
                SetLocalNumber(oNew, K2SE_SPAWN_INDEX, i);
                K2SE_SpawnReport(i, TRUE);
            }
            else
            {
                K2SE_SpawnReport(i, FALSE);
            }
        }
    }
}
