import bpy, addon_utils
addon_utils.enable("rigify", default_set=True, persistent=True)
bpy.ops.object.armature_human_metarig_add()
m = bpy.context.object
bpy.ops.object.mode_set(mode='EDIT')
face = [b for b in m.data.edit_bones if b.name=='face']
def kill(b):
    for c in list(b.children): kill(c)
    m.data.edit_bones.remove(b)
for b in face: kill(b)
bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.pose.rigify_generate()
rig = [o for o in bpy.data.objects if o.type=='ARMATURE' and o!=m][0]
d=[b for b in rig.data.bones if b.name.startswith('DEF-')]
print("DEF bones (no face):", len(d), "bbone>1:", sum(b.bbone_segments>1 for b in d))
